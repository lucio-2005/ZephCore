/*
 * ZephCore - LVGL UI access to contacts and remote management
 * SPDX-License-Identifier: MIT
 */

#include "ui_remote.h"

#include <CompanionMesh.h>
#include <errno.h>
#include <string.h>

static CompanionMesh *s_mesh;
static void (*s_kick_tx)(void);

#ifdef CONFIG_ZEPHCORE_UI_LVGL_DEMO_DATA
/* Demo build only: a few made-up contacts after the real ones, answered
 * locally after a short delay so the remote screens can be tried without a
 * mesh. Nothing here is stored or sent. */
#include <CayenneLPP.h>
#include <lvgl.h>
#include <ui_remote_events.h>
#include <stdio.h>

struct demo_node {
	const char *name;
	uint8_t type;
	uint32_t age_s;
	uint8_t path_len;
};

static const demo_node demo_nodes[] = {
	{"Monte Cavo", UI_NODE_REPEATER, 60, 0},
	{"Alice", UI_NODE_CHAT, 120, 1},
	{"Roma-Nord", UI_NODE_REPEATER, 22 * 60, 1},
	{"IT Mesh Room", UI_NODE_ROOM, 3600, 2},
	{"Garden", UI_NODE_SENSOR, 5 * 3600, 2},
	{"Bob", UI_NODE_CHAT, 15 * 60, 3},
};
#define DEMO_COUNT ((int)(sizeof(demo_nodes) / sizeof(demo_nodes[0])))

static void demo_key(int i, uint8_t *key)
{
	memset(key, 0x5A + i, UI_PUB_KEY_SIZE);
	key[0] = 0xDE;
	key[1] = (uint8_t)(0xD0 + i);
}

static int demo_index(const uint8_t *key, int len)
{
	if (len >= 2 && key[0] == 0xDE && key[1] >= 0xD0 && key[1] < 0xD0 + DEMO_COUNT) {
		return key[1] - 0xD0;
	}
	return -1;
}

static void demo_fill(int i, struct ui_contact *out)
{
	memset(out, 0, sizeof(*out));
	demo_key(i, out->pub_key);
	snprintf(out->name, sizeof(out->name), "%s", demo_nodes[i].name);
	out->type = demo_nodes[i].type;
	out->out_path_len = demo_nodes[i].path_len;
	out->age_s = demo_nodes[i].age_s;
	static const int32_t pos[][2] = {
		{41746200, 12704100}, {41893100, 12483000}, {41958800, 12511200},
		{41871400, 12560300}, {41922700, 12446900}, {41853900, 12402100},
	};

	out->lat_e6 = pos[i][0];
	out->lon_e6 = pos[i][1];
}

/* The one answer in flight. */
static struct {
	int node;
	enum ui_remote_event_type type;
	bool ok;
	uint8_t perms;
	uint32_t tag;
	uint8_t data[160];
	uint8_t len;
	char text[96];
} demo_ans;
static uint32_t demo_tag = 0x1000;

static void demo_deliver(lv_timer_t *t)
{
	uint8_t key[UI_PUB_KEY_SIZE];

	demo_key(demo_ans.node, key);
	if (demo_ans.type == UI_REMOTE_LOGIN_RESULT) {
		struct ui_remote_login_data d = {demo_ans.ok, demo_ans.perms, 0};

		ui_notify_remote_event(UI_REMOTE_LOGIN_RESULT, key, &d);
	} else if (demo_ans.type == UI_REMOTE_REQ_RESPONSE) {
		struct ui_remote_req_response_data d = {6, 4, demo_ans.data, demo_ans.len, demo_ans.tag};

		ui_notify_remote_event(UI_REMOTE_REQ_RESPONSE, key, &d);
	} else {
		struct ui_remote_cli_data d = {demo_ans.text};

		ui_notify_remote_event(UI_REMOTE_CLI_RESPONSE, key, &d);
	}
	lv_timer_delete(t);
}

static int demo_answer(int node, uint32_t *wait_ms)
{
	demo_ans.node = node;
	*wait_ms = 12000;
	lv_timer_t *t = lv_timer_create(demo_deliver, 900, NULL);

	lv_timer_set_repeat_count(t, 1);
	return 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF; }

static int demo_login(int node, const char *password, uint32_t *wait_ms)
{
	demo_ans.type = UI_REMOTE_LOGIN_RESULT;
	demo_ans.ok = password[0] == '\0' || strcmp(password, "admin") == 0;
	demo_ans.perms = strcmp(password, "admin") == 0 ? 1 : 0;
	return demo_answer(node, wait_ms);
}

static int demo_request(int node, const uint8_t *req, uint32_t *tag, uint32_t *wait_ms)
{
	uint8_t *d = demo_ans.data;

	demo_ans.type = UI_REMOTE_REQ_RESPONSE;
	demo_ans.tag = *tag = ++demo_tag;
	memset(d, 0, sizeof(demo_ans.data));
	switch (req[0]) {
	case 0x01:      /* status: RepeaterStats */
		put16(&d[0], 4087); put16(&d[2], 0); put16(&d[4], (uint16_t)-117); put16(&d[6], (uint16_t)-89);
		put32(&d[8], 18234); put32(&d[12], 9120); put32(&d[16], 1834); put32(&d[20], 4 * 86400 + 7 * 3600);
		put32(&d[24], 6011); put32(&d[28], 3109); put32(&d[32], 12002); put32(&d[36], 6232);
		put16(&d[40], 0); put16(&d[42], 26); put16(&d[44], 311); put16(&d[46], 2471);
		put32(&d[48], 5321); put32(&d[52], 12);
		demo_ans.len = demo_nodes[node].type == UI_NODE_ROOM ? 52 : 56;
		break;
	case 0x03: {    /* telemetry */
		CayenneLPP lpp(sizeof(demo_ans.data));

		lpp.addVoltage(1, 4.09f);
		lpp.addTemperature(1, 27.5f);
		if (demo_nodes[node].type == UI_NODE_SENSOR) {
			lpp.addTemperature(2, 18.4f);
			lpp.addRelativeHumidity(2, 63.5f);
			lpp.addBarometricPressure(2, 1012.8f);
			lpp.addLuminosity(3, 412);
		}
		memcpy(d, lpp.getBuffer(), lpp.getSize());
		demo_ans.len = lpp.getSize();
		break;
	}
	case 0x06: {    /* neighbours */
		static const uint32_t ago[] = {45, 600, 5400};
		static const int8_t snr[] = {38, 12, -22};
		int n = 0;

		for (int i = 0; i < DEMO_COUNT && n < 3; i++) {
			if (i == node || demo_nodes[i].type == UI_NODE_CHAT) {
				continue;
			}
			uint8_t key[UI_PUB_KEY_SIZE];

			demo_key(i, key);
			memcpy(&d[4 + n * 9], key, 4);
			put32(&d[4 + n * 9 + 4], ago[n]);
			d[4 + n * 9 + 8] = (uint8_t)snr[n];
			n++;
		}
		put16(&d[0], 7);
		put16(&d[2], (uint16_t)n);
		demo_ans.len = (uint8_t)(4 + n * 9);
		break;
	}
	case 0x07:      /* owner info */
		demo_ans.len = (uint8_t)snprintf((char *)d, sizeof(demo_ans.data), "v1.9.0 (demo)\n%s\nIW0ABC - demo node",
						 demo_nodes[node].name);
		break;
	default:
		demo_ans.len = 0;
		break;
	}
	return demo_answer(node, wait_ms);
}

static int demo_cli(int node, const char *cmd, uint32_t *wait_ms)
{
	static const struct {
		const char *cmd;
		const char *reply;
	} replies[] = {
		{"get lat", "> 41.746200"}, {"get lon", "> 12.704100"},
		{"get radio", "> 869.618,62.5,8,8"}, {"get tx", "> 22"}, {"get repeat", "> on"},
		{"get advert.interval", "> 120"}, {"get flood.advert.interval", "> 12"},
		{"get owner.info", "> IW0ABC - demo node"}, {"get guest.password", "> "},
		{"clock sync", "OK - clock set: 14:32:05 - 22/10/2026 UTC"}, {"advert", "OK - Advert sent"},
		{"reboot", "OK"}, {"ver", "v1.9.0 (demo)"},
	};

	demo_ans.type = UI_REMOTE_CLI_RESPONSE;
	snprintf(demo_ans.text, sizeof(demo_ans.text), "Unknown command");
	if (strcmp(cmd, "get name") == 0) {
		snprintf(demo_ans.text, sizeof(demo_ans.text), "> %s", demo_nodes[node].name);
	} else if (strncmp(cmd, "set ", 4) == 0 || strncmp(cmd, "password ", 9) == 0) {
		snprintf(demo_ans.text, sizeof(demo_ans.text), "OK (demo: not stored)");
	}
	for (const auto &r : replies) {
		if (strcmp(cmd, r.cmd) == 0) {
			snprintf(demo_ans.text, sizeof(demo_ans.text), "%s", r.reply);
		}
	}
	return demo_answer(node, wait_ms);
}
#endif /* CONFIG_ZEPHCORE_UI_LVGL_DEMO_DATA */

/* As the joystick UI: twice the one-way estimate plus 3 s, at least 12 s. */
static uint32_t answer_wait_ms(uint32_t est_timeout)
{
	uint32_t ms = est_timeout * 2 + 3000;

	return ms < 12000 ? 12000 : ms;
}

static void fill(const ContactInfo &c, struct ui_contact *out)
{
	uint32_t now = s_mesh->getRTCClock()->getCurrentTime();

	memcpy(out->pub_key, c.id.pub_key, UI_PUB_KEY_SIZE);
	memcpy(out->name, c.name, sizeof(out->name));
	out->name[sizeof(out->name) - 1] = '\0';
	out->type = c.type;
	out->out_path_len = c.out_path_len;
	out->age_s = (c.lastmod && now >= c.lastmod) ? now - c.lastmod : UINT32_MAX;
	out->lat_e6 = c.gps_lat;
	out->lon_e6 = c.gps_lon;
}

extern "C" void ui_remote_init(void *companion_mesh, void (*kick_tx)(void))
{
	s_mesh = static_cast<CompanionMesh *>(companion_mesh);
	s_kick_tx = kick_tx;
}

extern "C" int ui_contacts_count(void)
{
	int n = s_mesh ? s_mesh->getNumContacts() : 0;
#ifdef CONFIG_ZEPHCORE_UI_LVGL_DEMO_DATA
	n += DEMO_COUNT;
#endif
	return n;
}

extern "C" bool ui_contacts_get(int idx, struct ui_contact *out)
{
	ContactInfo c;

#ifdef CONFIG_ZEPHCORE_UI_LVGL_DEMO_DATA
	int real = s_mesh ? s_mesh->getNumContacts() : 0;

	if (idx >= real && idx < real + DEMO_COUNT) {
		demo_fill(idx - real, out);
		return true;
	}
#endif
	if (!s_mesh || idx < 0 || !s_mesh->getContactByIdx((uint32_t)idx, c)) {
		return false;
	}
	fill(c, out);
	return true;
}

extern "C" bool ui_contacts_find(const uint8_t *pub_key, int prefix_len, struct ui_contact *out)
{
#ifdef CONFIG_ZEPHCORE_UI_LVGL_DEMO_DATA
	if (demo_index(pub_key, prefix_len) >= 0) {
		demo_fill(demo_index(pub_key, prefix_len), out);
		return true;
	}
#endif
	ContactInfo *c = s_mesh ? s_mesh->lookupContactByPubKey(pub_key, prefix_len) : nullptr;

	if (!c) {
		return false;
	}
	fill(*c, out);
	return true;
}

static int sent(int result, uint32_t est, uint32_t *wait_ms)
{
	if (result == MSG_SEND_FAILED) {
		return -EIO;
	}
	*wait_ms = answer_wait_ms(est);
	if (s_kick_tx) {
		s_kick_tx();
	}
	return 0;
}

extern "C" int ui_remote_login(const uint8_t *pub_key, const char *password, uint32_t *wait_ms)
{
	ContactInfo *c = s_mesh ? s_mesh->lookupContactByPubKey(pub_key, PUB_KEY_SIZE) : nullptr;
	uint32_t est = 0;

#ifdef CONFIG_ZEPHCORE_UI_LVGL_DEMO_DATA
	if (demo_index(pub_key, UI_PUB_KEY_SIZE) >= 0) {
		return demo_login(demo_index(pub_key, UI_PUB_KEY_SIZE), password, wait_ms);
	}
#endif
	if (!c) {
		return -ENOENT;
	}
	int r = s_mesh->sendLogin(*c, password, est);

	if (r != MSG_SEND_FAILED) {
		s_mesh->setUiLoginPending(c->id.pub_key);
	}
	return sent(r, est, wait_ms);
}

extern "C" int ui_remote_request(const uint8_t *pub_key, const uint8_t *req, uint8_t len, uint32_t *tag,
				 uint32_t *wait_ms)
{
	ContactInfo *c = s_mesh ? s_mesh->lookupContactByPubKey(pub_key, PUB_KEY_SIZE) : nullptr;
	uint32_t est = 0;

#ifdef CONFIG_ZEPHCORE_UI_LVGL_DEMO_DATA
	if (demo_index(pub_key, UI_PUB_KEY_SIZE) >= 0) {
		return demo_request(demo_index(pub_key, UI_PUB_KEY_SIZE), req, tag, wait_ms);
	}
#endif
	if (!c) {
		return -ENOENT;
	}
	int r = s_mesh->sendRequest(*c, req, len, *tag, est);

	if (r != MSG_SEND_FAILED) {
		s_mesh->setUiAdminTag(*tag);
	}
	return sent(r, est, wait_ms);
}

extern "C" int ui_remote_cli(const uint8_t *pub_key, const char *cmd, uint32_t *wait_ms)
{
	ContactInfo *c = s_mesh ? s_mesh->lookupContactByPubKey(pub_key, PUB_KEY_SIZE) : nullptr;
	uint32_t est = 0;

#ifdef CONFIG_ZEPHCORE_UI_LVGL_DEMO_DATA
	if (demo_index(pub_key, UI_PUB_KEY_SIZE) >= 0) {
		return demo_cli(demo_index(pub_key, UI_PUB_KEY_SIZE), cmd, wait_ms);
	}
#endif
	if (!c) {
		return -ENOENT;
	}
	/* TXT_TYPE_CLI_DATA, as the joystick UI: understood by servers of any version. */
	uint32_t ts = s_mesh->getRTCClock()->getCurrentTimeUnique();

	return sent(s_mesh->sendCommandData(*c, ts, 0, TXT_TYPE_CLI_DATA, cmd, est), est, wait_ms);
}

extern "C" void ui_remote_cancel(void)
{
	if (s_mesh) {
		s_mesh->clearUiLoginPending();
		s_mesh->clearUiAdminTag();
	}
}

extern "C" int ui_self_telemetry(uint8_t *out)
{
	return s_mesh ? s_mesh->uiSelfTelemetry(out) : 0;
}
