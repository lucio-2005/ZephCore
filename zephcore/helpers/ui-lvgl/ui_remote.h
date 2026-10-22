/*
 * ZephCore - LVGL UI access to contacts and remote management
 * SPDX-License-Identifier: MIT
 *
 * The contact table, and the requests the touch UI sends to repeaters, room
 * servers and sensors: login, binary requests and CLI commands. Answers come
 * back through ui_notify_remote_event() (helpers/ui/ui_remote_events.h).
 * Main thread only.
 */

#ifndef ZEPHCORE_UI_REMOTE_H
#define ZEPHCORE_UI_REMOTE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_PUB_KEY_SIZE   32
#define UI_SELF_TELEM_MAX 180   /* >= MAX_PACKET_PAYLOAD - 4 */

/* ADV_TYPE_* */
#define UI_NODE_CHAT      1
#define UI_NODE_REPEATER  2
#define UI_NODE_ROOM      3
#define UI_NODE_SENSOR    4

struct ui_contact {
	uint8_t pub_key[UI_PUB_KEY_SIZE];
	char name[32];
	uint8_t type;
	uint8_t out_path_len;   /* 0xFF: no path yet, messages flood */
	uint32_t age_s;         /* since last heard; UINT32_MAX if unknown */
	int32_t lat_e6;
	int32_t lon_e6;
};

void ui_remote_init(void *companion_mesh, void (*kick_tx)(void));

int ui_contacts_count(void);
bool ui_contacts_get(int idx, struct ui_contact *out);
bool ui_contacts_find(const uint8_t *pub_key, int prefix_len, struct ui_contact *out);

/* 0 when sent, -ENOENT when the contact is gone, -EIO when it could not be
 * queued. *wait_ms is how long the answer may take. */
int ui_remote_login(const uint8_t *pub_key, const char *password, uint32_t *wait_ms);
int ui_remote_request(const uint8_t *pub_key, const uint8_t *req, uint8_t len, uint32_t *tag,
		      uint32_t *wait_ms);
int ui_remote_cli(const uint8_t *pub_key, const char *cmd, uint32_t *wait_ms);
/* Forget the pending login and request, so a late answer goes nowhere. */
void ui_remote_cancel(void);

/* This node's own telemetry (CayenneLPP); returns its length. */
int ui_self_telemetry(uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHCORE_UI_REMOTE_H */
