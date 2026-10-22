/*
 * ZephCore - LVGL touch UI: Contacts tab and remote management
 * SPDX-License-Identifier: MIT
 *
 * The Contacts tab lists the contact table, newest first. A companion opens a
 * chat; a repeater, room server or sensor opens the remote screen: log in,
 * then status, telemetry, neighbours and owner info, and as admin a console,
 * the node's settings, clock sync, advert and reboot, as the MeshCore app
 * offers them.
 *
 * One request is in flight at a time; the rest wait in a short queue. An
 * answer is matched by the node's key and, for binary requests, the tag
 * (CompanionMesh keeps the UI's login and tag apart from the app's). The wait
 * restarts when the request actually goes on air.
 *
 * Main thread only: ui_notify_remote_event() comes from CompanionMesh.
 */

#include <zephyr/kernel.h>
#include <lvgl.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ui_task.h"
#include "ui_remote_events.h"
#include "ui_remote.h"
#include "ui_lvgl_priv.h"
#include "mcui_gen.h"
#include "remote_parse.h"
#include "telemetry_text.h"

#define CONTACT_ROWS_MAX	40
#define JOBS_MAX		16
#define CONSOLE_MAX		8
#define CMD_LEN			100
#define REPLY_LEN		160
#define PASSWORD_LEN		16	/* upstream: 15 characters */
#define NEIGHBOURS_ASK		14	/* fits the repeater's 130-byte reply */
#define TELEM_FIELDS_MAX	16

enum job_kind {
	J_NONE,
	J_LOGIN,
	J_STATUS,
	J_TELEMETRY,
	J_NEIGHBOURS,
	J_INFO,
	J_CLI_ACTION,	/* a button: clock sync, advert, reboot */
	J_CLI_CONSOLE,
	J_CLI_GET,	/* a remote setting, into its subject */
	J_CLI_SET,
};

struct job {
	uint8_t kind;
	int8_t rset;		/* remote setting index for GET/SET */
	char text[CMD_LEN];	/* CLI command, or the action's title */
};

/* Remote settings: read with "get", changed with "set" (CommonCLI). */
struct rset {
	const char *key;
	const char *get;	/* NULL: write-only */
	const char *set_fmt;
	lv_subject_t **subject;
	const char *title;
	const char *hint;
	bool number;
	uint8_t max_len;
};

static const struct rset rsets[] = {
	{"r.name", "get name", "set name %s", &mcui_rs_name, "Name", "Shown to other nodes", false, 31},
	{"r.lat", "get lat", "set lat %s", &mcui_rs_lat, "Latitude", "Decimal degrees, -90 to 90", true, 11},
	{"r.lon", "get lon", "set lon %s", &mcui_rs_lon, "Longitude", "Decimal degrees, -180 to 180", true, 11},
	{"r.radio", "get radio", "set radio %s", &mcui_rs_radio, "Radio",
	 "MHz,kHz,SF,CR e.g. 869.618,62.5,8,8", false, 40},
	{"r.tx", "get tx", "set tx %s", &mcui_rs_tx, "TX power (dBm)", "As the radio allows", true, 3},
	{"r.repeat", "get repeat", "set repeat %s", &mcui_rs_repeat, "Repeat", "on or off", false, 3},
	{"r.advert.interval", "get advert.interval", "set advert.interval %s", &mcui_rs_adv,
	 "Zero-hop advert (min)", "Minutes; 0 turns it off", true, 3},
	{"r.flood.advert.interval", "get flood.advert.interval", "set flood.advert.interval %s",
	 &mcui_rs_fadv, "Flood advert (hours)", "Hours, 3 to 168; 0 turns it off", true, 3},
	{"r.owner.info", "get owner.info", "set owner.info %s", &mcui_rs_owner, "Owner info",
	 "Anyone can read it. | starts a new line", false, 100},
	{"r.guest.password", "get guest.password", "set guest.password %s", &mcui_rs_guest,
	 "Guest password", "Empty: guests need no password", false, 15},
	{"r.password", NULL, "password %s", &mcui_rs_admin, "Admin password",
	 "The password to log in as admin", false, 15},
};

/* Contacts tab */
static uint8_t row_keys[CONTACT_ROWS_MAX][UI_PUB_KEY_SIZE];
static uint32_t contacts_sig;

/* The node on the remote screen */
static struct ui_contact peer;
static lv_obj_t *remote_scr;
static bool logged_in;
static bool is_admin;
static char password[PASSWORD_LEN];

/* Requests */
static struct job jobs[JOBS_MAX];
static int jobs_head, jobs_count;
static struct job cur_job;
static uint32_t cur_tag;
static bool awaiting_tx;
static lv_timer_t *wait_timer;

/* Console */
static struct console_line {
	char cmd[CMD_LEN];
	char reply[REPLY_LEN];
} console[CONSOLE_MAX];
static int console_head, console_count;
static lv_obj_t *console_panel;
static lv_obj_t *rsettings_panel;

static void run_next(void);

static bool peer_is(const uint8_t *prefix)
{
	return peer.name[0] && memcmp(peer.pub_key, prefix, 4) == 0;
}

static const char *type_name(uint8_t type)
{
	switch (type) {
	case UI_NODE_CHAT: return "Companion";
	case UI_NODE_REPEATER: return "Repeater";
	case UI_NODE_ROOM: return "Room server";
	case UI_NODE_SENSOR: return "Sensor";
	default: return "Node";
	}
}

/* ========== Contacts tab ========== */

static void fmt_age(char *out, size_t len, uint32_t age_s)
{
	if (age_s == UINT32_MAX) {
		snprintf(out, len, "not heard yet");
	} else if (age_s < 60) {
		snprintf(out, len, "heard just now");
	} else if (age_s < 3600) {
		snprintf(out, len, "heard %um ago", (unsigned)(age_s / 60));
	} else if (age_s < 86400) {
		snprintf(out, len, "heard %uh ago", (unsigned)(age_s / 3600));
	} else {
		snprintf(out, len, "heard %ud ago", (unsigned)(age_s / 86400));
	}
}

static void fmt_path(char *out, size_t len, uint8_t out_path_len)
{
	uint8_t hops = out_path_len & 63;

	if (out_path_len == 0xFF) {
		snprintf(out, len, "Flood");
	} else if (hops == 0) {
		snprintf(out, len, "Direct");
	} else {
		snprintf(out, len, "%u hop%s", hops, hops == 1 ? "" : "s");
	}
}

void ui_lvgl_contacts_refresh(bool force)
{
	lv_obj_t *list = lv_obj_find_by_name(mcui_home, "contact_list");
	int filter = lv_subject_get_int(mcui_contacts_filter);
	int n = ui_contacts_count();
	int best_idx[CONTACT_ROWS_MAX];
	uint32_t best_age[CONTACT_ROWS_MAX];
	int shown = 0, matched = 0;
	struct ui_contact c;
	char buf[64];

	if (!list) {
		return;
	}
	/* The newest CONTACT_ROWS_MAX of the type asked for, newest first. */
	for (int i = 0; i < n; i++) {
		if (!ui_contacts_get(i, &c) || (filter && c.type != filter)) {
			continue;
		}
		matched++;
		int pos = shown;

		while (pos > 0 && best_age[pos - 1] > c.age_s) {
			pos--;
		}
		if (pos >= CONTACT_ROWS_MAX) {
			continue;
		}
		int last = shown < CONTACT_ROWS_MAX ? shown : CONTACT_ROWS_MAX - 1;

		memmove(&best_idx[pos + 1], &best_idx[pos], (size_t)(last - pos) * sizeof(best_idx[0]));
		memmove(&best_age[pos + 1], &best_age[pos], (size_t)(last - pos) * sizeof(best_age[0]));
		best_idx[pos] = i;
		best_age[pos] = c.age_s;
		if (shown < CONTACT_ROWS_MAX) {
			shown++;
		}
	}

	/* Rebuilding resets the scroll position: only when the list changed. */
	uint32_t sig = (uint32_t)n * 2654435761u ^ (uint32_t)filter;

	for (int i = 0; i < shown; i++) {
		sig = sig * 31u + (uint32_t)best_idx[i];
	}
	if (!force && sig == contacts_sig) {
		return;
	}
	contacts_sig = sig;

	if (matched == 0) {
		snprintf(buf, sizeof(buf), "%s", n ? "None of this type" : "No contacts yet");
	} else if (matched > shown) {
		snprintf(buf, sizeof(buf), "Newest %d of %d", shown, matched);
	} else {
		snprintf(buf, sizeof(buf), "%d contact%s", matched, matched == 1 ? "" : "s");
	}
	lv_subject_set_string(mcui_contacts_summary, buf);

	lv_obj_clean(list);
	for (int i = 0; i < shown; i++) {
		static const char *const kind[] = {"?", "CHAT", "RPT", "ROOM", "SENSOR"};
		char letter[2], heard[24], path[16];
		lv_color_t tint;

		if (!ui_contacts_get(best_idx[i], &c)) {
			continue;
		}
		switch (c.type) {
		case UI_NODE_REPEATER: tint = MCUI_KIND_RPT; break;
		case UI_NODE_ROOM: tint = MCUI_KIND_ROOM; break;
		case UI_NODE_SENSOR: tint = MCUI_KIND_SENSOR; break;
		default: tint = MCUI_KIND_CHAT; break;
		}
		letter[0] = c.name[0] ? c.name[0] : '?';
		letter[1] = '\0';
		if (letter[0] >= 'a' && letter[0] <= 'z') {
			letter[0] = (char)(letter[0] - 'a' + 'A');
		}
		fmt_age(heard, sizeof(heard), c.age_s);
		fmt_path(path, sizeof(path), c.out_path_len);
		lv_obj_t *row = mcui_contact_row_create(list, c.name, letter, kind[c.type <= 4 ? c.type : 0],
							tint, heard, path, "");

		memcpy(row_keys[i], c.pub_key, UI_PUB_KEY_SIZE);
		lv_obj_set_user_data(row, (void *)(uintptr_t)(i + 1));
	}
	if (n == 0) {
		lv_obj_t *empty = lv_label_create(list);

		lv_label_set_text(empty, "Nodes appear here as their adverts are heard.");
		lv_obj_set_width(empty, lv_pct(100));
		lv_obj_add_style(empty, &mcui_style_text_muted, 0);
	}
}

static void filter_changed(lv_observer_t *observer, lv_subject_t *subject)
{
	ARG_UNUSED(observer);
	ARG_UNUSED(subject);
	ui_lvgl_contacts_refresh(true);
}

void ui_on_contact_open(lv_event_t *e)
{
	uintptr_t slot = (uintptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
	struct ui_contact c;

	if (slot == 0 || slot > CONTACT_ROWS_MAX) {
		return;		/* a previewer sample row */
	}
	if (!ui_contacts_find(row_keys[slot - 1], UI_PUB_KEY_SIZE, &c)) {
		ui_lvgl_toast(true, "That contact was removed");
		ui_lvgl_contacts_refresh(true);
		return;
	}
	if (c.type == UI_NODE_CHAT) {
		char path[24];

		fmt_path(path, sizeof(path), c.out_path_len);
		ui_lvgl_open_chat(c.name, path);
		return;
	}
	if (memcmp(c.pub_key, peer.pub_key, UI_PUB_KEY_SIZE) != 0) {
		/* Another node: start over. */
		ui_remote_cancel();
		lv_timer_pause(wait_timer);
		jobs_count = 0;
		cur_job.kind = J_NONE;
		logged_in = false;
		is_admin = false;
		password[0] = '\0';
		console_count = 0;
		lv_subject_set_string(mcui_remote_result_title, "");
		lv_subject_set_string(mcui_remote_state, "");
	}
	peer = c;
	ui_lvgl_load_new_screen(mcui_remote_create());
}

/* ========== Remote screen ========== */

static void show(lv_obj_t *scr, const char *name, bool on)
{
	lv_obj_t *o = scr ? lv_obj_find_by_name(scr, name) : NULL;

	if (o) {
		lv_obj_set_hidden(o, !on);
	}
}

static void remote_update(void)
{
	char buf[64];
	bool server = peer.type == UI_NODE_REPEATER || peer.type == UI_NODE_ROOM;

	snprintf(buf, sizeof(buf), "%s | %s", type_name(peer.type),
		 !logged_in ? "not logged in" : is_admin ? "admin" : "guest");
	lv_subject_set_string(mcui_remote_role, buf);
	lv_subject_set_string(mcui_remote_pwd_txt, password[0] ? "set" : "none (guest)");
	lv_subject_set_string(mcui_remote_name, peer.name);
	if (!remote_scr) {
		return;
	}
	show(remote_scr, "remote_login", !logged_in);
	show(remote_scr, "act_neighbours", peer.type == UI_NODE_REPEATER);
	show(remote_scr, "act_chat", peer.type == UI_NODE_ROOM);
	show(remote_scr, "act_console", is_admin);
	show(remote_scr, "act_settings", is_admin);
	show(remote_scr, "act_clock", is_admin);
	show(remote_scr, "act_advert", is_admin && server);
	show(remote_scr, "act_reboot", is_admin);
}

static lv_obj_t *rows_clear(void)
{
	lv_obj_t *rows = remote_scr ? lv_obj_find_by_name(remote_scr, "remote_rows") : NULL;

	if (rows) {
		lv_obj_clean(rows);
	}
	return rows;
}

static void row(lv_obj_t *rows, const char *label, const char *fmt, ...)
{
	char value[64];
	va_list ap;

	if (!rows) {
		return;
	}
	va_start(ap, fmt);
	vsnprintf(value, sizeof(value), fmt, ap);
	va_end(ap);
	mcui_info_row_create(rows, label, value);
}

static void remote_screen_deleted(lv_event_t *e)
{
	ARG_UNUSED(e);
	remote_scr = NULL;
}

void ui_on_remote_open(lv_event_t *e)
{
	lv_obj_t *scr = lv_event_get_current_target_obj(e);

	if (remote_scr != scr) {
		remote_scr = scr;
		lv_obj_add_event_cb(scr, remote_screen_deleted, LV_EVENT_DELETE, NULL);
	}
	rows_clear();
	remote_update();
}

static bool save_login_password(const char *text, const char **err)
{
	ARG_UNUSED(err);
	snprintf(password, sizeof(password), "%s", text);
	remote_update();
	return true;
}

static bool queue(uint8_t kind, int rset, const char *text)
{
	if (jobs_count >= JOBS_MAX) {
		ui_lvgl_toast(true, "Too many requests waiting");
		return false;
	}
	struct job *j = &jobs[(jobs_head + jobs_count) % JOBS_MAX];

	j->kind = kind;
	j->rset = (int8_t)rset;
	snprintf(j->text, sizeof(j->text), "%s", text ? text : "");
	jobs_count++;
	run_next();
	return true;
}

void ui_on_remote_login(lv_event_t *e)
{
	ARG_UNUSED(e);
	queue(J_LOGIN, -1, NULL);
}

static void console_open(void);
static void rsettings_open(void);

void ui_on_remote_action(lv_event_t *e)
{
	const char *what = lv_event_get_user_data(e);
	static int64_t reboot_armed;

	if (!what) {
		return;
	}
	if (strcmp(what, "status") == 0) {
		queue(J_STATUS, -1, NULL);
	} else if (strcmp(what, "telemetry") == 0) {
		queue(J_TELEMETRY, -1, NULL);
	} else if (strcmp(what, "neighbours") == 0) {
		queue(J_NEIGHBOURS, -1, NULL);
	} else if (strcmp(what, "info") == 0) {
		queue(J_INFO, -1, NULL);
	} else if (strcmp(what, "chat") == 0) {
		ui_lvgl_open_chat(peer.name, "Room server");
	} else if (strcmp(what, "console") == 0) {
		console_open();
	} else if (strcmp(what, "settings") == 0) {
		rsettings_open();
	} else if (strcmp(what, "clock") == 0) {
		/* The node takes our clock from the command's timestamp. */
		queue(J_CLI_ACTION, -1, "clock sync");
	} else if (strcmp(what, "advert") == 0) {
		queue(J_CLI_ACTION, -1, "advert");
	} else if (strcmp(what, "reboot") == 0) {
		if (k_uptime_get() < reboot_armed) {
			reboot_armed = 0;
			queue(J_CLI_ACTION, -1, "reboot");
		} else {
			reboot_armed = k_uptime_get() + 4000;
			ui_lvgl_toast(false, "Tap Reboot again to restart the node");
		}
	}
}

/* ========== Requests ========== */

static const char *job_title(const struct job *j)
{
	switch (j->kind) {
	case J_LOGIN: return "LOGIN";
	case J_STATUS: return "STATUS";
	case J_TELEMETRY: return "TELEMETRY";
	case J_NEIGHBOURS: return "NEIGHBOURS";
	case J_INFO: return "OWNER INFO";
	case J_CLI_ACTION: return "COMMAND";
	default: return NULL;
	}
}

static void set_state(const char *state)
{
	lv_subject_set_string(mcui_remote_state, state);
}

/* A new answer starts at its top. */
static void reveal_result(void)
{
	lv_obj_t *rows = remote_scr ? lv_obj_find_by_name(remote_scr, "remote_rows") : NULL;

	if (rows) {
		lv_obj_scroll_to_y(rows, 0, LV_ANIM_OFF);
	}
}

static void rs_state_update(void)
{
	char buf[48];
	int left = jobs_count + (cur_job.kind == J_CLI_GET ? 1 : 0);

	if (!rsettings_panel) {
		return;
	}
	for (int i = 0; i < jobs_count; i++) {
		if (jobs[(jobs_head + i) % JOBS_MAX].kind != J_CLI_GET) {
			left--;
		}
	}
	if (left > 0) {
		snprintf(buf, sizeof(buf), "Reading settings, %d to go...", left);
	} else {
		snprintf(buf, sizeof(buf), "Tap a value to change it.");
	}
	lv_subject_set_string(mcui_rs_state, buf);
}

static void console_add(const char *cmd);
static void console_reply(const char *reply);

/* The current job ended: with an answer, or none (data NULL). */
static void finish(void)
{
	cur_job.kind = J_NONE;
	awaiting_tx = false;
	lv_timer_pause(wait_timer);
	ui_remote_cancel();
	rs_state_update();
	run_next();
}

static void run_next(void)
{
	uint32_t wait_ms = 12000;
	int rc = 0;
	uint8_t req[8];

	if (cur_job.kind != J_NONE) {
		return;
	}
	do {
		if (jobs_count == 0) {
			return;
		}
		cur_job = jobs[jobs_head];
		jobs_head = (jobs_head + 1) % JOBS_MAX;
		jobs_count--;
	} while (cur_job.kind == J_NONE);	/* dropped by ui_on_rsettings_close() */

	const char *title = job_title(&cur_job);

	if (title && cur_job.kind != J_LOGIN) {
		lv_subject_set_string(mcui_remote_result_title, title);
		rows_clear();
	}
	switch (cur_job.kind) {
	case J_LOGIN:
		rc = ui_remote_login(peer.pub_key, password, &wait_ms);
		break;
	case J_STATUS:
	case J_TELEMETRY:
	case J_INFO:
		req[0] = cur_job.kind == J_STATUS ? REMOTE_REQ_STATUS :
			 cur_job.kind == J_TELEMETRY ? REMOTE_REQ_TELEMETRY : REMOTE_REQ_OWNER_INFO;
		rc = ui_remote_request(peer.pub_key, req, 1, &cur_tag, &wait_ms);
		break;
	case J_NEIGHBOURS:
		rc = ui_remote_request(peer.pub_key, req, remote_neighbours_request(req, NEIGHBOURS_ASK),
				       &cur_tag, &wait_ms);
		break;
	default:
		rc = ui_remote_cli(peer.pub_key, cur_job.text, &wait_ms);
		break;
	}
	if (rc != 0) {
		ui_lvgl_toast(true, rc == -ENOENT ? "That contact was removed" : "Could not send: radio busy");
		if (cur_job.kind == J_CLI_CONSOLE) {
			console_reply("(not sent)");
		}
		set_state("not sent");
		cur_job.kind = J_NONE;
		jobs_count = 0;
		rs_state_update();
		return;
	}
	set_state(cur_job.kind == J_LOGIN ? "logging in..." : "waiting...");
	awaiting_tx = true;
	lv_timer_set_period(wait_timer, wait_ms);
	lv_timer_reset(wait_timer);
	lv_timer_resume(wait_timer);
}

void ui_lvgl_remote_packet_sent(void)
{
	/* The wait counts from when the request went on air, not from the queue. */
	if (awaiting_tx && cur_job.kind != J_NONE) {
		awaiting_tx = false;
		lv_timer_reset(wait_timer);
	}
}

static void wait_expired(lv_timer_t *t)
{
	char msg[64];

	ARG_UNUSED(t);
	if (cur_job.kind == J_NONE) {
		lv_timer_pause(wait_timer);
		return;
	}
	switch (cur_job.kind) {
	case J_LOGIN:
		snprintf(msg, sizeof(msg), "No answer from %s", peer.name);
		ui_lvgl_toast(true, msg);
		set_state("");
		break;
	case J_CLI_CONSOLE:
		console_reply("(no answer)");
		break;
	case J_CLI_GET:
		lv_subject_set_string(*rsets[cur_job.rset].subject, "-");
		break;
	case J_CLI_SET:
		ui_lvgl_toast(true, "No answer: the change may not be saved");
		break;
	default:
		set_state(logged_in ? "no answer" : "no answer: log in first?");
		break;
	}
	finish();
}

static void show_status(const uint8_t *d, uint8_t len)
{
	struct remote_status s;
	lv_obj_t *rows = rows_clear();
	char a[16], b[16];

	if (!remote_status_parse(d, len, peer.type == UI_NODE_ROOM, &s)) {
		row(rows, "Status", "not understood");
		return;
	}
	remote_fmt_duration(s.uptime_s, a, sizeof(a));
	row(rows, "Uptime", "%s", a);
	row(rows, "Battery", "%u.%02u V", s.batt_mv / 1000, (s.batt_mv % 1000) / 10);
	if (s.len < 48) {
		return;
	}
	row(rows, "Noise floor", "%d dBm", s.noise_floor);
	row(rows, "Last RSSI / SNR", "%d dBm / %.1f dB", s.last_rssi, (double)s.last_snr_x4 / 4.0);
	row(rows, "Packets in / out", "%u / %u", (unsigned)s.pkts_recv, (unsigned)s.pkts_sent);
	row(rows, "Flood in / out", "%u / %u", (unsigned)s.recv_flood, (unsigned)s.sent_flood);
	row(rows, "Direct in / out", "%u / %u", (unsigned)s.recv_direct, (unsigned)s.sent_direct);
	row(rows, "Dups flood / direct", "%u / %u", s.flood_dups, s.direct_dups);
	remote_fmt_duration(s.air_time_s, a, sizeof(a));
	if (peer.type == UI_NODE_ROOM) {
		row(rows, "Airtime TX", "%s", a);
		row(rows, "Posts / pushed", "%u / %u", s.posted, s.post_pushes);
	} else {
		remote_fmt_duration(s.rx_air_time_s, b, sizeof(b));
		row(rows, "Airtime TX / RX", "%s / %s", a, b);
		if (s.len >= 56) {
			row(rows, "Receive errors", "%u", (unsigned)s.recv_errors);
		}
	}
	row(rows, "TX queue", "%u", s.tx_queue);
	if (s.err_events) {
		row(rows, "Error flags", "%04X", s.err_events);
	}
}

static void show_telemetry(lv_obj_t *rows, const uint8_t *d, uint8_t len, const char *self_name)
{
	struct telemetry_field f[TELEM_FIELDS_MAX];
	int n = telemetry_fields(d, len, f, TELEM_FIELDS_MAX);
	int last_ch = -1;
	bool several = false;

	for (int i = 1; i < n; i++) {
		several |= f[i].channel != f[0].channel;
	}
	if (n == 0) {
		row(rows, "Telemetry", "none shared");
		return;
	}
	for (int i = 0; i < n; i++) {
		if (several && f[i].channel != last_ch) {
			char head[24];

			if (f[i].channel == 1) {
				snprintf(head, sizeof(head), "%s", self_name);
			} else {
				snprintf(head, sizeof(head), "SENSOR %u", f[i].channel);
			}
			mcui_section_label_create(rows, head);
			last_ch = f[i].channel;
		}
		mcui_info_row_create(rows, f[i].label, f[i].value);
	}
}

static void show_neighbours(const uint8_t *d, uint8_t len)
{
	struct remote_neighbour nb[NEIGHBOURS_ASK];
	lv_obj_t *rows = rows_clear();
	int total = 0;
	int n = remote_neighbours_parse(d, len, &total, nb, NEIGHBOURS_ASK);
	char name[40], age[16];

	if (n < 0) {
		row(rows, "Neighbours", "not understood");
		return;
	}
	if (n == 0) {
		row(rows, "Neighbours", "none heard yet");
		return;
	}
	for (int i = 0; i < n; i++) {
		struct ui_contact c;

		if (ui_contacts_find(nb[i].prefix, 4, &c)) {
			snprintf(name, sizeof(name), "%s", c.name);
		} else {
			snprintf(name, sizeof(name), "%02X%02X%02X%02X", nb[i].prefix[0], nb[i].prefix[1],
				 nb[i].prefix[2], nb[i].prefix[3]);
		}
		remote_fmt_duration(nb[i].heard_secs_ago, age, sizeof(age));
		row(rows, name, "%s ago | SNR %.1f", age, (double)nb[i].snr_x4 / 4.0);
	}
	if (total > n) {
		row(rows, "", "%d more not shown", total - n);
	}
}

static void show_owner_info(const uint8_t *d, uint8_t len)
{
	char buf[REPLY_LEN + 1];
	const char *fw, *name, *owner;
	lv_obj_t *rows = rows_clear();

	if (len > REPLY_LEN) {
		len = REPLY_LEN;
	}
	remote_owner_info_parse(d, len, buf, &fw, &name, &owner);
	row(rows, "Firmware", "%s", fw);
	row(rows, "Name", "%s", name);
	row(rows, "Owner", "%s", owner[0] ? owner : "-");
}

/* "> value" is a get's answer; anything else (an error) shows as "-". */
static const char *cli_value(const char *reply)
{
	if (reply[0] == '>' && reply[1] == ' ') {
		return reply + 2;
	}
	if (reply[0] == '>') {
		return reply + 1;
	}
	return NULL;
}

static void cli_answer(const char *text)
{
	switch (cur_job.kind) {
	case J_CLI_CONSOLE:
		console_reply(text);
		break;
	case J_CLI_GET: {
		const char *v = cli_value(text);

		lv_subject_set_string(*rsets[cur_job.rset].subject, v ? (v[0] ? v : "(empty)") : "-");
		break;
	}
	case J_CLI_SET:
		if (strncmp(text, "OK", 2) == 0 || strncmp(text, "password now", 12) == 0) {
			ui_lvgl_toast(false, strncmp(text, "OK - ", 5) == 0 ? text + 5 : "Saved");
			const struct rset *r = &rsets[cur_job.rset];

			if (r->get) {
				/* Read back what the node kept. */
				struct job j = cur_job;

				finish();
				queue(J_CLI_GET, j.rset, r->get);
				return;
			}
		} else {
			ui_lvgl_toast(true, text);
		}
		break;
	default: {
		lv_obj_t *rows = rows_clear();

		row(rows, cur_job.text, "%s", text);
		set_state("");
		reveal_result();
		break;
	}
	}
	finish();
}

void ui_notify_remote_event(enum ui_remote_event_type type, const uint8_t *prefix, const void *payload)
{
	if (cur_job.kind == J_NONE || !peer_is(prefix)) {
		return;
	}
	if (type == UI_REMOTE_LOGIN_RESULT && cur_job.kind == J_LOGIN) {
		const struct ui_remote_login_data *d = payload;
		char msg[64];

		if (d->success) {
			logged_in = true;
			is_admin = d->permissions != 0;
			snprintf(msg, sizeof(msg), "Logged in to %s%s", peer.name, is_admin ? " as admin" : "");
			ui_lvgl_toast(false, msg);
		} else {
			ui_lvgl_toast(true, "Login refused: wrong password?");
		}
		set_state("");
		remote_update();
		finish();
	} else if (type == UI_REMOTE_REQ_RESPONSE) {
		const struct ui_remote_req_response_data *d = payload;

		if (d->tag != cur_tag || !d->data) {
			return;
		}
		switch (cur_job.kind) {
		case J_STATUS:
			show_status(d->data, d->data_len);
			break;
		case J_TELEMETRY:
			show_telemetry(rows_clear(), d->data, d->data_len, "NODE");
			break;
		case J_NEIGHBOURS:
			show_neighbours(d->data, d->data_len);
			break;
		case J_INFO:
			show_owner_info(d->data, d->data_len);
			break;
		default:
			return;
		}
		set_state("just now");
		reveal_result();
		finish();
	} else if (type == UI_REMOTE_CLI_RESPONSE && cur_job.kind >= J_CLI_ACTION) {
		const struct ui_remote_cli_data *d = payload;

		cli_answer(d->text ? d->text : "");
	}
	ui_lvgl_wake();
}

/* ========== Console ========== */

static void console_render(void)
{
	lv_obj_t *list = console_panel ? lv_obj_find_by_name(console_panel, "console_list") : NULL;
	char cmd[CMD_LEN + 2];

	if (!list) {
		return;
	}
	lv_obj_clean(list);
	for (int i = 0; i < console_count; i++) {
		const struct console_line *l = &console[(console_head + i) % CONSOLE_MAX];

		snprintf(cmd, sizeof(cmd), "> %s", l->cmd);
		mcui_console_entry_create(list, cmd, l->reply);
	}
	if (console_count == 0) {
		mcui_console_entry_create(list, "Commands go to this node; its replies show here.",
					  "Try: ver, get radio, neighbors, clock");
	}
	lv_obj_update_layout(list);
	lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void console_add(const char *cmd)
{
	struct console_line *l;

	if (console_count < CONSOLE_MAX) {
		l = &console[(console_head + console_count++) % CONSOLE_MAX];
	} else {
		l = &console[console_head];
		console_head = (console_head + 1) % CONSOLE_MAX;
	}
	snprintf(l->cmd, sizeof(l->cmd), "%s", cmd);
	snprintf(l->reply, sizeof(l->reply), "...");
	console_render();
}

/* The reply belongs to the oldest command still showing "...". */
static void console_reply(const char *reply)
{
	for (int i = 0; i < console_count; i++) {
		struct console_line *l = &console[(console_head + i) % CONSOLE_MAX];

		if (strcmp(l->reply, "...") == 0) {
			snprintf(l->reply, sizeof(l->reply), "%s", reply);
			break;
		}
	}
	console_render();
}

static void console_send(void)
{
	lv_obj_t *ta = console_panel ? lv_obj_find_by_name(console_panel, "console_input") : NULL;
	const char *text = ta ? lv_textarea_get_text(ta) : "";

	if (!text[0]) {
		return;
	}
	if (queue(J_CLI_CONSOLE, -1, text)) {
		console_add(text);
		lv_textarea_set_text(ta, "");
	}
}

void ui_on_console_send(lv_event_t *e)
{
	ARG_UNUSED(e);
	console_send();
}

void ui_on_console_close(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (console_panel) {
		lv_obj_delete_async(console_panel);
		console_panel = NULL;
	}
}

static void console_kbd_event(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_READY) {
		console_send();
	} else {
		ui_on_console_close(e);
	}
}

static void console_open(void)
{
	if (console_panel) {
		return;
	}
	console_panel = mcui_console_panel_create(lv_layer_top());

	lv_obj_t *ta = lv_obj_find_by_name(console_panel, "console_input");
	lv_obj_t *kbd = lv_obj_find_by_name(console_panel, "console_kbd");

	if (ta && kbd) {
		lv_textarea_set_max_length(ta, CMD_LEN - 1);
		lv_keyboard_set_textarea(kbd, ta);
		lv_obj_add_state(ta, LV_STATE_FOCUSED);
		lv_obj_add_event_cb(kbd, console_kbd_event, LV_EVENT_READY, NULL);
		lv_obj_add_event_cb(kbd, console_kbd_event, LV_EVENT_CANCEL, NULL);
	}
	console_render();
}

/* ========== Remote settings ========== */

static void rsettings_load(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(rsets); i++) {
		lv_subject_set_string(*rsets[i].subject, rsets[i].get ? "..." : "change");
	}
	for (size_t i = 0; i < ARRAY_SIZE(rsets); i++) {
		if (rsets[i].get) {
			queue(J_CLI_GET, (int)i, rsets[i].get);
		}
	}
	rs_state_update();
}

static void rsettings_open(void)
{
	if (rsettings_panel) {
		return;
	}
	rsettings_panel = mcui_rsettings_panel_create(lv_layer_top());
	rsettings_load();
}

void ui_on_rsettings_reload(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (jobs_count == 0 && cur_job.kind != J_CLI_GET) {
		rsettings_load();
	}
}

void ui_on_rsettings_close(lv_event_t *e)
{
	ARG_UNUSED(e);
	/* Drop the reads still queued; one in flight finishes on its own. */
	for (int i = 0; i < jobs_count; i++) {
		struct job *j = &jobs[(jobs_head + i) % JOBS_MAX];

		if (j->kind == J_CLI_GET) {
			j->kind = J_NONE;
		}
	}
	if (rsettings_panel) {
		lv_obj_delete_async(rsettings_panel);
		rsettings_panel = NULL;
	}
}

static const struct rset *edit_rset;

static bool save_rset(const char *text, const char **err)
{
	char cmd[CMD_LEN];

	if (!edit_rset) {
		return true;
	}
	if (strchr(text, '\n')) {
		*err = "One line only";
		return false;
	}
	snprintf(cmd, sizeof(cmd), edit_rset->set_fmt, text);
	queue(J_CLI_SET, (int)(edit_rset - rsets), cmd);
	lv_subject_set_string(*edit_rset->subject, "saving...");
	return true;
}

void ui_lvgl_remote_edit(const char *key)
{
	if (strcmp(key, "r.login") == 0) {
		ui_lvgl_edit_open(&(struct ui_edit_req){
			.title = "Password",
			.hint = "Admin or guest password of this node",
			.initial = password,
			.password = true,
			.max_len = PASSWORD_LEN - 1,
			.save = save_login_password,
		});
		return;
	}
	for (size_t i = 0; i < ARRAY_SIZE(rsets); i++) {
		if (strcmp(rsets[i].key, key) != 0) {
			continue;
		}
		const char *cur = lv_subject_get_string(*rsets[i].subject);
		bool known = rsets[i].get && strcmp(cur, "-") && strcmp(cur, "...") && strcmp(cur, "(empty)");

		edit_rset = &rsets[i];
		ui_lvgl_edit_open(&(struct ui_edit_req){
			.title = rsets[i].title,
			.hint = rsets[i].hint,
			.initial = known ? cur : "",
			.number = rsets[i].number,
			.max_len = rsets[i].max_len,
			.save = save_rset,
		});
		return;
	}
}

/* ========== This node's sensors ========== */

static void sensors_fill(lv_obj_t *scr)
{
	uint8_t buf[UI_SELF_TELEM_MAX];
	struct telemetry_field f[TELEM_FIELDS_MAX];
	lv_obj_t *rows = lv_obj_find_by_name(scr, "sensor_rows");
	int len = ui_self_telemetry(buf);
	int n = telemetry_fields(buf, (uint8_t)len, f, TELEM_FIELDS_MAX);
	char sum[64];

	if (rows) {
		lv_obj_clean(rows);
		show_telemetry(rows, buf, (uint8_t)len, "THIS NODE");
	}
	if (n > 0 && strcmp(f[0].label, "Battery") == 0) {
		snprintf(sum, sizeof(sum), "Battery %s | %d reading%s", f[0].value, n, n == 1 ? "" : "s");
	} else {
		snprintf(sum, sizeof(sum), "%d reading%s", n, n == 1 ? "" : "s");
	}
	lv_subject_set_string(mcui_sum_sensors, sum);
}

void ui_on_sensors_open(lv_event_t *e)
{
	sensors_fill(lv_event_get_current_target_obj(e));
}

void ui_on_sensors_refresh(lv_event_t *e)
{
	ARG_UNUSED(e);
	sensors_fill(lv_screen_active());
	ui_lvgl_toast(false, "Sensors read again");
}

/* ========== Init ========== */

void ui_lvgl_remote_init(void)
{
	wait_timer = lv_timer_create(wait_expired, 12000, NULL);
	lv_timer_set_auto_delete(wait_timer, false);
	lv_timer_pause(wait_timer);
	lv_subject_add_observer(mcui_contacts_filter, filter_changed, NULL);
	lv_subject_set_string(mcui_remote_result_title, "");
	lv_subject_set_string(mcui_remote_state, "");
	lv_subject_set_string(mcui_sum_sensors, "Battery, MCU temperature, GPS, sensors");
}
