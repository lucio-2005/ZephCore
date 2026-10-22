/*
 * ZephCore - Messages sent from an on-device UI
 * SPDX-License-Identifier: MIT
 *
 * Direct messages are retried until ACKed (the last attempt forces a flood)
 * and channel messages wait a feedback window to learn whether a repeater
 * re-sent them. Only then is the message mirrored to the companion app,
 * with the outcome in its body. Shared by the UIs that can send.
 *
 * Main thread only, except the wake hook, which runs from a timer ISR.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set by the UI that owns sending. dm_done / channel_done run on the main
 * thread from ui_outbox_process(), before the app mirror is queued. */
struct ui_outbox_hooks {
	void (*wake)(void);  /* any context: call ui_outbox_process() soon */
	void (*dm_done)(uint32_t timestamp, const char *recipient_name, bool delivered);
	void (*channel_done)(int cookie, uint32_t timestamp, bool heard);
};

enum ui_send_result {
	UI_SEND_OK = 0,
	UI_SEND_QUEUE_FULL,    /* all slots of that kind are waiting for an outcome */
	UI_SEND_NO_RECIPIENT,  /* no contact or channel with that name */
	UI_SEND_FAILED,        /* the mesh refused the message */
};

#if IS_ENABLED(CONFIG_ZEPHCORE_UI_OUTBOX)

/**
 * Bind the outbox to the mesh. kick_tx wakes the main loop to drain the
 * send queue (MESH_EVENT_TX_DRAIN). Call once before the event loop.
 */
void ui_outbox_init(void *companion_mesh, void *rtc_clock, void (*kick_tx)(void));

void ui_outbox_set_hooks(const struct ui_outbox_hooks *hooks);

/** Run retries and finished feedback windows. Main thread, after hooks->wake(). */
void ui_outbox_process(void);

/**
 * Match an incoming ACK to a pending DM. On a match the DM completes as
 * delivered and the recipient's 6-byte key prefix is written to out_pubkey.
 */
bool ui_outbox_try_match_ack(uint32_t ack, uint8_t out_pubkey[6]);

/**
 * Send text to the contact or channel with this exact name. On UI_SEND_OK,
 * *timestamp is the message's timestamp, which dm_done / channel_done
 * report back; cookie is passed through to channel_done.
 */
enum ui_send_result ui_outbox_send_dm_to(const char *contact_name, const char *text,
					 uint32_t *timestamp);
enum ui_send_result ui_outbox_send_channel_to(const char *channel_name, const char *text,
					      int cookie, uint32_t *timestamp);

#else

static inline bool ui_outbox_try_match_ack(uint32_t ack, uint8_t out_pubkey[6])
{
	ARG_UNUSED(ack);
	ARG_UNUSED(out_pubkey);
	return false;
}

#endif /* CONFIG_ZEPHCORE_UI_OUTBOX */

#ifdef __cplusplus
}

#if IS_ENABLED(CONFIG_ZEPHCORE_UI_OUTBOX)
#include <helpers/ContactInfo.h>
#include <helpers/ChannelDetails.h>

/* For a UI that already holds the contact / channel. */
bool ui_outbox_has_free_dm_slot(void);
bool ui_outbox_has_free_channel_slot(void);
uint32_t ui_outbox_new_timestamp(void);
bool ui_outbox_start_dm(ContactInfo &recipient, uint32_t timestamp, const char *text);
bool ui_outbox_start_channel(uint8_t channel_idx, ChannelDetails &ch, uint32_t timestamp,
			     const char *text, int cookie);
#endif
#endif /* __cplusplus */
