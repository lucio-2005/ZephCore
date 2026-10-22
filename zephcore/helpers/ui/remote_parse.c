/*
 * SPDX-License-Identifier: MIT
 * See remote_parse.h.
 */

#include "remote_parse.h"

#include <stdio.h>
#include <string.h>

static uint16_t u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool remote_status_parse(const uint8_t *d, uint8_t len, bool is_room, struct remote_status *s)
{
	memset(s, 0, sizeof(*s));
	if (len < 4) {
		return false;
	}
	s->len = len;
#define F16(off, field) do { if (len >= (off) + 2) s->field = u16(&d[off]); } while (0)
#define F32(off, field) do { if (len >= (off) + 4) s->field = u32(&d[off]); } while (0)
	F16(0, batt_mv);
	F16(2, tx_queue);
	if (len >= 6) s->noise_floor = (int16_t)u16(&d[4]);
	if (len >= 8) s->last_rssi = (int16_t)u16(&d[6]);
	F32(8, pkts_recv);
	F32(12, pkts_sent);
	F32(16, air_time_s);
	F32(20, uptime_s);
	F32(24, sent_flood);
	F32(28, sent_direct);
	F32(32, recv_flood);
	F32(36, recv_direct);
	F16(40, err_events);
	if (len >= 44) s->last_snr_x4 = (int16_t)u16(&d[42]);
	F16(44, direct_dups);
	F16(46, flood_dups);
	if (is_room) {
		F16(48, posted);
		F16(50, post_pushes);
	} else {
		F32(48, rx_air_time_s);
		F32(52, recv_errors);
	}
#undef F16
#undef F32
	return true;
}

uint8_t remote_neighbours_request(uint8_t *req, uint8_t count)
{
	req[0] = REMOTE_REQ_NEIGHBOURS;
	req[1] = 0;             /* request version */
	req[2] = count;
	req[3] = 0;             /* offset, u16 */
	req[4] = 0;
	req[5] = 0;             /* newest first */
	req[6] = 4;             /* key prefix length */
	return 7;
}

int remote_neighbours_parse(const uint8_t *d, uint8_t len, int *total,
			    struct remote_neighbour *out, int max)
{
	if (len < 4) {
		return -1;
	}
	*total = (int16_t)u16(&d[0]);
	int returned = (int16_t)u16(&d[2]);
	int n = 0;
	unsigned pos = 4;

	if (returned < 0) {
		return -1;
	}
	while (n < returned && n < max && pos + 9 <= len) {
		memcpy(out[n].prefix, &d[pos], 4);
		out[n].heard_secs_ago = u32(&d[pos + 4]);
		out[n].snr_x4 = (int8_t)d[pos + 8];
		pos += 9;
		n++;
	}
	return n;
}

void remote_owner_info_parse(const uint8_t *d, uint8_t len, char *buf,
			     const char **firmware, const char **name, const char **owner)
{
	const char **fields[3] = {firmware, name, owner};
	int fi = 0;

	memcpy(buf, d, len);
	buf[len] = '\0';
	*firmware = buf;
	*name = "";
	*owner = "";
	for (char *p = buf; *p && fi < 2; p++) {
		if (*p == '\n') {
			*p = '\0';
			*fields[++fi] = p + 1;
		}
	}
}

void remote_fmt_duration(uint32_t s, char *out, unsigned cap)
{
	if (s >= 86400) {
		snprintf(out, cap, "%ud %uh", (unsigned)(s / 86400), (unsigned)(s % 86400 / 3600));
	} else if (s >= 3600) {
		snprintf(out, cap, "%uh %um", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
	} else if (s >= 60) {
		snprintf(out, cap, "%um %us", (unsigned)(s / 60), (unsigned)(s % 60));
	} else {
		snprintf(out, cap, "%us", (unsigned)s);
	}
}
