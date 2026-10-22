/*
 * ZephCore - Messages sent from an on-device UI
 * SPDX-License-Identifier: MIT
 */

#include "ui_outbox.h"

#include <CompanionMesh.h>
#include <mesh/MeshCore.h>
#include <zephyr/kernel.h>
#include <string.h>

namespace {

constexpr int MAX_PENDING_SENDS = 4;
constexpr uint8_t MAX_SEND_ATTEMPTS = 5;  /* attempt 4 (0-indexed) forces flood */
constexpr int MAX_PENDING_CHANNEL_SENDS = 4;
constexpr uint32_t CHANNEL_FEEDBACK_WINDOW_MS = 5000;

struct PendingSend {
	bool active;
	volatile bool retry_due;     /* set by timer ISR, consumed in ui_outbox_process() */
	bool delivered;
	bool failed;
	uint8_t recipient_pubkey[PUB_KEY_SIZE];
	char recipient_name[32];
	uint32_t expected_ack;
	uint32_t timestamp;
	char text[MAX_TEXT_LEN + 1];
	uint8_t attempt;
	uint32_t timeout_ms;
	struct k_timer retry_timer;
};

struct PendingChannelSend {
	bool active;
	volatile bool feedback_due;
	uint8_t channel_idx;
	int cookie;
	uint32_t timestamp;
	uint32_t hash;
	char text[MAX_TEXT_LEN + 1];
	struct k_timer feedback_timer;
};

CompanionMesh *s_mesh;
mesh::RTCClock *s_rtc;
void (*s_kick_tx)(void);
struct ui_outbox_hooks s_hooks;
PendingSend s_sends[MAX_PENDING_SENDS];
PendingChannelSend s_channel_sends[MAX_PENDING_CHANNEL_SENDS];

void wake(void)
{
	if (s_hooks.wake) {
		s_hooks.wake();
	}
}

void retry_timer_cb(struct k_timer *t)
{
	PendingSend *slot = (PendingSend *)k_timer_user_data_get(t);

	slot->retry_due = true;
	wake();
}

void feedback_timer_cb(struct k_timer *t)
{
	PendingChannelSend *slot = (PendingChannelSend *)k_timer_user_data_get(t);

	slot->feedback_due = true;
	wake();
}

int alloc_send_slot(void)
{
	for (int i = 0; i < MAX_PENDING_SENDS; i++) {
		if (!s_sends[i].active) {
			return i;
		}
	}
	return -1;
}

int alloc_channel_slot(void)
{
	for (int i = 0; i < MAX_PENDING_CHANNEL_SENDS; i++) {
		if (!s_channel_sends[i].active) {
			return i;
		}
	}
	return -1;
}

void schedule_retry(PendingSend &s)
{
	k_timer_stop(&s.retry_timer);
	k_timer_start(&s.retry_timer, K_MSEC(s.timeout_ms), K_NO_WAIT);
}

void complete_send(PendingSend &s)
{
	k_timer_stop(&s.retry_timer);
	bool delivered = s.delivered;

	if (s_hooks.dm_done) {
		s_hooks.dm_done(s.timestamp, s.recipient_name, delivered);
	}

	/* Now (and only now) mirror to the app with the outcome prefix. */
	ContactInfo *recipient = s_mesh ? s_mesh->lookupContactByPubKey(s.recipient_pubkey, PUB_KEY_SIZE) : nullptr;
	if (recipient) {
		s_mesh->queueLocalSentContactMessage(*recipient, s.timestamp, s.text, delivered);
	}

	s.active = false;
	s.retry_due = false;
}

void do_send(PendingSend &s)
{
	ContactInfo *recipient = s_mesh ? s_mesh->lookupContactByPubKey(s.recipient_pubkey, PUB_KEY_SIZE) : nullptr;
	if (!recipient) {
		s.failed = true;
		complete_send(s);
		return;
	}

	/* The last attempt clears the saved path so it floods. The clear persists
	 * so future DMs to this contact also re-discover. */
	if (s.attempt == MAX_SEND_ATTEMPTS - 1) {
		recipient->out_path_len = OUT_PATH_UNKNOWN;
		s_mesh->markContactsDirtyPublic();
	}

	uint32_t expected_ack = 0, est_timeout = 0;
	int result = s_mesh->sendMessage(*recipient, s.timestamp, s.attempt, s.text,
					 expected_ack, est_timeout);
	if (result == 0) {
		/* sendMessage rejected (e.g. queue full) — back off and try again. */
		s.timeout_ms = 3000;
		schedule_retry(s);
		return;
	}
	if (s_kick_tx) {
		s_kick_tx();
	}
	s.expected_ack = expected_ack;
	/* Allow the ACK round-trip (2× est_timeout + small grace) before retrying. */
	s.timeout_ms = (est_timeout > 0) ? (est_timeout * 2 + 3000) : 8000;
	schedule_retry(s);
}

void complete_channel_send(PendingChannelSend &s, bool heard)
{
	if (s_hooks.channel_done) {
		s_hooks.channel_done(s.cookie, s.timestamp, heard);
	}

	/* Mirror to the app now (deferred until the outcome is known) with the
	 * heard/unheard prefix. */
	if (s_mesh) {
		s_mesh->queueLocalSentChannelMessage(s.channel_idx, s.timestamp, s.text, heard);
	}

	s.active = false;
	s.feedback_due = false;
}

void copy_text(char *dst, const char *text)
{
	size_t len = strlen(text);

	if (len > MAX_TEXT_LEN) {
		len = MAX_TEXT_LEN;
	}
	memcpy(dst, text, len);
	dst[len] = '\0';
}

} /* namespace */

extern "C" void ui_outbox_init(void *companion_mesh, void *rtc_clock, void (*kick_tx)(void))
{
	s_mesh = static_cast<CompanionMesh *>(companion_mesh);
	s_rtc = static_cast<mesh::RTCClock *>(rtc_clock);
	s_kick_tx = kick_tx;
	for (int i = 0; i < MAX_PENDING_SENDS; i++) {
		s_sends[i].active = false;
		s_sends[i].retry_due = false;
		k_timer_init(&s_sends[i].retry_timer, retry_timer_cb, NULL);
		k_timer_user_data_set(&s_sends[i].retry_timer, &s_sends[i]);
	}
	for (int i = 0; i < MAX_PENDING_CHANNEL_SENDS; i++) {
		s_channel_sends[i].active = false;
		s_channel_sends[i].feedback_due = false;
		k_timer_init(&s_channel_sends[i].feedback_timer, feedback_timer_cb, NULL);
		k_timer_user_data_set(&s_channel_sends[i].feedback_timer, &s_channel_sends[i]);
	}
}

extern "C" void ui_outbox_set_hooks(const struct ui_outbox_hooks *hooks)
{
	s_hooks = *hooks;
}

extern "C" void ui_outbox_process(void)
{
	for (int i = 0; i < MAX_PENDING_SENDS; i++) {
		PendingSend &s = s_sends[i];
		if (!s.active || !s.retry_due) continue;
		s.retry_due = false;
		if (s.delivered) {
			complete_send(s);
			continue;
		}
		if (s.attempt + 1 >= MAX_SEND_ATTEMPTS) {
			/* Out of retries. */
			s.failed = true;
			complete_send(s);
			continue;
		}
		s.attempt++;
		do_send(s);
	}

	for (int i = 0; i < MAX_PENDING_CHANNEL_SENDS; i++) {
		PendingChannelSend &s = s_channel_sends[i];
		if (!s.active || !s.feedback_due) continue;
		s.feedback_due = false;
		int dupe_count = s_mesh ? s_mesh->getContentionTracker().extractDupeCount(s.hash) : -1;
		complete_channel_send(s, dupe_count > 0);
	}
}

extern "C" bool ui_outbox_try_match_ack(uint32_t ack, uint8_t out_pubkey[6])
{
	if (ack == 0) return false;
	for (int i = 0; i < MAX_PENDING_SENDS; i++) {
		PendingSend &s = s_sends[i];
		if (!s.active || s.expected_ack != ack) continue;
		s.delivered = true;
		ContactInfo *recipient = s_mesh ? s_mesh->lookupContactByPubKey(s.recipient_pubkey, PUB_KEY_SIZE) : nullptr;
		complete_send(s);
		if (!recipient) return false;
		memcpy(out_pubkey, recipient->id.pub_key, 6);
		return true;
	}
	return false;
}

bool ui_outbox_has_free_dm_slot(void)
{
	return alloc_send_slot() >= 0;
}

bool ui_outbox_has_free_channel_slot(void)
{
	return alloc_channel_slot() >= 0;
}

uint32_t ui_outbox_new_timestamp(void)
{
	return s_rtc ? s_rtc->getCurrentTimeUnique() : k_uptime_get_32();
}

bool ui_outbox_start_dm(ContactInfo &recipient, uint32_t timestamp, const char *text)
{
	if (!text || !s_mesh) return false;
	int slot_idx = alloc_send_slot();
	if (slot_idx < 0) return false;

	PendingSend &s = s_sends[slot_idx];
	s.active = true;
	s.retry_due = false;
	s.delivered = false;
	s.failed = false;
	memcpy(s.recipient_pubkey, recipient.id.pub_key, PUB_KEY_SIZE);
	strncpy(s.recipient_name, recipient.name, sizeof(s.recipient_name) - 1);
	s.recipient_name[sizeof(s.recipient_name) - 1] = '\0';
	s.timestamp = timestamp;
	copy_text(s.text, text);
	s.attempt = 0;
	s.expected_ack = 0;
	s.timeout_ms = 0;

	do_send(s);
	return true;
}

bool ui_outbox_start_channel(uint8_t channel_idx, ChannelDetails &ch, uint32_t timestamp,
			     const char *text, int cookie)
{
	if (!text || !s_mesh) return false;
	int slot_idx = alloc_channel_slot();
	if (slot_idx < 0) return false;

	uint32_t pkt_hash = 0;
	int text_len = (int)strlen(text);
	bool ok = s_mesh->sendGroupMessage(timestamp, ch.channel, s_mesh->prefs.node_name, text,
					   text_len, &pkt_hash);
	if (!ok) return false;
	if (s_kick_tx) {
		s_kick_tx();
	}

	PendingChannelSend &s = s_channel_sends[slot_idx];
	s.active = true;
	s.feedback_due = false;
	s.channel_idx = channel_idx;
	s.cookie = cookie;
	s.timestamp = timestamp;
	s.hash = pkt_hash;
	copy_text(s.text, text);

	k_timer_stop(&s.feedback_timer);
	k_timer_start(&s.feedback_timer, K_MSEC(CHANNEL_FEEDBACK_WINDOW_MS), K_NO_WAIT);
	return true;
}

extern "C" enum ui_send_result ui_outbox_send_dm_to(const char *contact_name, const char *text,
						    uint32_t *timestamp)
{
	if (!s_mesh) return UI_SEND_FAILED;
	ContactInfo found;
	ContactInfo *recipient = nullptr;

	for (int i = 0; i < s_mesh->getNumContacts(); i++) {
		if (s_mesh->getContactByIdx(i, found) && strcmp(found.name, contact_name) == 0) {
			recipient = s_mesh->lookupContactByPubKey(found.id.pub_key, PUB_KEY_SIZE);
			break;
		}
	}
	if (!recipient) return UI_SEND_NO_RECIPIENT;
	if (!ui_outbox_has_free_dm_slot()) return UI_SEND_QUEUE_FULL;

	uint32_t ts = ui_outbox_new_timestamp();

	if (!ui_outbox_start_dm(*recipient, ts, text)) return UI_SEND_FAILED;
	*timestamp = ts;
	return UI_SEND_OK;
}

extern "C" enum ui_send_result ui_outbox_send_channel_to(const char *channel_name, const char *text,
							 int cookie, uint32_t *timestamp)
{
	if (!s_mesh) return UI_SEND_FAILED;
	ChannelDetails ch;
	int idx = -1;

	for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
		if (s_mesh->getChannel(i, ch) && ch.name[0] && strcmp(ch.name, channel_name) == 0) {
			idx = i;
			break;
		}
	}
	if (idx < 0) return UI_SEND_NO_RECIPIENT;
	if (!ui_outbox_has_free_channel_slot()) return UI_SEND_QUEUE_FULL;

	uint32_t ts = ui_outbox_new_timestamp();

	if (!ui_outbox_start_channel((uint8_t)idx, ch, ts, text, cookie)) return UI_SEND_FAILED;
	*timestamp = ts;
	return UI_SEND_OK;
}
