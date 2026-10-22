/*
 * SPDX-License-Identifier: MIT
 *
 * Responses to the requests an on-device UI sends a repeater, room server or
 * sensor (the response after its 4-byte tag), as upstream's servers build
 * them (app/RepeaterMesh.*, app/RoomServerMesh.cpp). Little-endian, short
 * replies keep what they carry. No Zephyr dependencies (host-tested).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTE_REQ_STATUS      0x01
#define REMOTE_REQ_TELEMETRY   0x03
#define REMOTE_REQ_NEIGHBOURS  0x06
#define REMOTE_REQ_OWNER_INFO  0x07

/* RepeaterStats / ServerStats: the first 48 bytes are common; repeaters then
 * send RX airtime and receive errors, room servers posts and pushes. */
struct remote_status {
	uint8_t len;                    /* bytes received; fields past it are 0 */
	uint16_t batt_mv;
	uint16_t tx_queue;
	int16_t noise_floor;
	int16_t last_rssi;
	uint32_t pkts_recv;
	uint32_t pkts_sent;
	uint32_t air_time_s;
	uint32_t uptime_s;
	uint32_t sent_flood;
	uint32_t sent_direct;
	uint32_t recv_flood;
	uint32_t recv_direct;
	uint16_t err_events;
	int16_t last_snr_x4;
	uint16_t direct_dups;
	uint16_t flood_dups;
	uint32_t rx_air_time_s;         /* repeater */
	uint32_t recv_errors;           /* repeater */
	uint16_t posted;                /* room server */
	uint16_t post_pushes;           /* room server */
};

/* is_room: the node advertised as a room server. False if too short to
 * carry the battery and queue. */
bool remote_status_parse(const uint8_t *data, uint8_t len, bool is_room, struct remote_status *out);

struct remote_neighbour {
	uint8_t prefix[4];
	uint32_t heard_secs_ago;
	int8_t snr_x4;
};

/* The 7-byte request for up to count neighbours, most recently heard first,
 * with 4-byte key prefixes. Returns its length. */
uint8_t remote_neighbours_request(uint8_t *req, uint8_t count);

/* Returns entries parsed (<= max), or -1 if malformed; *total is how many the
 * node knows. */
int remote_neighbours_parse(const uint8_t *data, uint8_t len, int *total,
			    struct remote_neighbour *out, int max);

/* Owner info: "firmware\nname\nowner". Splits a copy into the three fields
 * (missing ones are ""). buf must hold len + 1 bytes. */
void remote_owner_info_parse(const uint8_t *data, uint8_t len, char *buf,
			     const char **firmware, const char **name, const char **owner);

/* "3d 4h", "5h 12m", "7m 3s", "42s" */
void remote_fmt_duration(uint32_t secs, char *out, unsigned cap);

#ifdef __cplusplus
}
#endif
