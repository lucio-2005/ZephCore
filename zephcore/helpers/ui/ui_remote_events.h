/*
 * ZephCore - remote management events for on-device UIs
 * SPDX-License-Identifier: MIT
 *
 * CompanionMesh -> the active UI frontend, on the main thread: the result of
 * a login the UI sent, CLI replies, responses to requests, and node discover
 * answers. Implemented by the joystick UI and the LVGL UI
 * (CONFIG_ZEPHCORE_UI_REMOTE); inline no-ops otherwise.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ui_remote_event_type {
	UI_REMOTE_LOGIN_RESULT  = 0,
	UI_REMOTE_CLI_RESPONSE  = 1,
	UI_REMOTE_REQ_RESPONSE  = 2,
	UI_REMOTE_DISCOVER_RESP = 3,
};

struct ui_remote_login_data {
	bool success;
	uint8_t permissions;
	uint32_t server_time;
};

struct ui_remote_cli_data {
	const char *text;
};

struct ui_remote_req_response_data {
	int8_t snr_local;
	int8_t snr_remote;
	const uint8_t *data;    /* the response after its 4-byte tag */
	uint8_t data_len;
	uint32_t tag;           /* to match the request's tag */
};

struct ui_remote_discover_data {
	int8_t snr;
	int8_t snr_remote;
	uint8_t path_len;
};

#if IS_ENABLED(CONFIG_ZEPHCORE_UI_REMOTE)

void ui_notify_remote_event(enum ui_remote_event_type type, const uint8_t *pub_key_prefix,
			    const void *payload);

#else

static inline void ui_notify_remote_event(enum ui_remote_event_type type,
					  const uint8_t *pub_key_prefix, const void *payload)
{
	ARG_UNUSED(type);
	ARG_UNUSED(pub_key_prefix);
	ARG_UNUSED(payload);
}

#endif

#ifdef __cplusplus
}
#endif
