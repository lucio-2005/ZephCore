/*
 * ZephCore - LVGL touch UI
 * SPDX-License-Identifier: MIT
 *
 * ui_task.h implementation for colour touch displays. The layout comes from
 * xml/ (LVGL XML), converted to C in generated/ by scripts/lvgl_xml2c.py.
 *
 * Threading: the ui_* setters may run on any thread. They write a model under
 * a spinlock, mark it dirty and wake the main loop. ui_lvgl_run() copies the
 * model and touches LVGL on the main thread only. Settings changed on screen
 * go back to the mesh through ui_mesh_actions.
 *
 * Wakeups: a k_timer armed with LVGL's next deadline, plus an input callback.
 * The pointer read timer is paused while the screen is untouched, so an idle
 * UI does not wake the main loop.
 *
 * Problems are shown three ways: a banner for ongoing conditions (radio not
 * ready), the Alerts screen behind the status-bar badge (every logged error
 * and warning, through a log backend, plus the conditions above), and a toast
 * for the result of an action the user just took.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#ifdef CONFIG_LOG
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>
#endif
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "ui_task.h"
#include "ui_mesh_actions.h"
#include "ui_outbox.h"
#include "ui_lvgl.h"
#include "mcui_gen.h"

#ifdef CONFIG_ZEPHCORE_UI_BUZZER
#include "buzzer.h"
#endif

LOG_MODULE_REGISTER(ui_lvgl, LOG_LEVEL_INF);

#define RECENT_MAX	8
#define MSG_MAX		32
#define NAME_LEN	32
#define TEXT_LEN	160
#define CONV_MAX	16
#define OUT_PATH_UNKNOWN 0xFF
/* Same "time is set" threshold as the other UIs (2025-01-01). */
#define EPOCH_VALID	1735689600u
/* Keep reading the pointer this long after the last input event. */
#define INPUT_IDLE_MS	300
#define ALERT_MAX	16
#define ALERT_TEXT_LEN	96
#define TOAST_MS	3000

enum alert_level {
	ALERT_NONE = 0,
	ALERT_WARN = 1,
	ALERT_ERR = 2,
};

enum {
	D_NODE = BIT(0), D_CLOCK = BIT(1), D_BATT = BIT(2), D_RADIO = BIT(3),
	D_LINK = BIT(4), D_SETTINGS = BIT(5), D_RECENT = BIT(6), D_MSGS = BIT(7),
	D_COUNT = BIT(8), D_ALERTS = BIT(9), D_BANNER = BIT(10),
};

/* Outgoing message state, from ui_outbox. */
enum msg_status {
	MSG_RECEIVED = 0,
	MSG_SENDING,
	MSG_DELIVERED,
	MSG_NOT_DELIVERED,
	MSG_HEARD,
	MSG_NOT_HEARD,
};

struct ui_msg {
	uint32_t seq;
	uint32_t epoch;
	uint32_t out_id;	/* outgoing: matches channel_done's cookie */
	uint8_t path_len;
	uint8_t status;
	bool channel;
	char conv[NAME_LEN];
	char from[NAME_LEN];
	char text[TEXT_LEN];
};

struct ui_recent {
	char name[16];
	int16_t rssi;
	uint32_t age_s;
};

struct ui_alert {
	uint32_t seq;
	uint32_t epoch;
	uint32_t uptime_s;
	uint16_t count;
	uint8_t level;
	char source[24];
	char text[ALERT_TEXT_LEN];
};

struct ui_model {
	char node_name[NAME_LEN];
	uint32_t epoch;
	int64_t epoch_at_ms;
	int8_t tz;
	uint16_t batt_mv;
	uint8_t batt_pct;
	uint32_t freq_hz;
	uint8_t sf;
	uint16_t bw_x10;
	uint8_t cr;
	int8_t tx_power;
	int16_t noise_floor;
	uint8_t links;
	bool app_connected;
	bool gps_enabled;
	bool ble_enabled;
	uint8_t buzzer_mode;
	uint16_t msg_count;
	uint8_t recent_count;
	struct ui_recent recent[RECENT_MAX];
	uint32_t msg_seq;
	struct ui_msg msgs[MSG_MAX];
	bool radio_known;
	bool radio_ready;
	bool gps_available;
	uint32_t alert_seq;
	uint32_t alert_seen_seq;
	uint8_t alert_count;
	struct ui_alert alerts[ALERT_MAX];
};

/* Written by any thread under model_lock; view is the main thread's copy. */
static struct ui_model model;
static struct ui_model view;
static struct k_spinlock model_lock;
static atomic_t dirty;
static atomic_t input_seen;

static void (*signal_fn)(void);
static struct k_timer lvgl_timer;
static int64_t last_input_ms;
static bool initialized;
static bool unblanked;
/* Set while mesh state is pushed into subjects, so the observers below do not
 * send it straight back to the mesh. */
static bool applying;

/* Main-thread conversation state. */
struct conv_read {
	char name[NAME_LEN];
	uint32_t read_seq;
};
static struct conv_read conv_reads[CONV_MAX];
static lv_obj_t *conv_screen;
static char conv_name[NAME_LEN];
static bool conv_is_channel;
static uint32_t conv_shown_seq;
static int out_id_next;
static lv_obj_t *alerts_screen;
static lv_timer_t *toast_timer;

static void wake(void)
{
	void (*fn)(void) = signal_fn;

	if (fn) {
		fn();
	}
}

static void mark(uint32_t bits)
{
	atomic_or(&dirty, bits);
	wake();
}

static void copy_str(char *dst, size_t len, const char *src)
{
	if (!src) {
		src = "";
	}
	strncpy(dst, src, len - 1);
	dst[len - 1] = '\0';
}

/* ========== Alerts (any thread) ========== */

static uint32_t model_now_epoch(const struct ui_model *m);

/* Record a problem; a repeat of the newest identical entry only bumps its count. */
static void add_alert_locked(enum alert_level level, const char *source, const char *text)
{
	for (int i = 0; i < model.alert_count; i++) {
		struct ui_alert *a = &model.alerts[i];

		if (a->level == level && strcmp(a->source, source) == 0 &&
		    strncmp(a->text, text, sizeof(a->text) - 1) == 0) {
			a->count++;
			a->seq = ++model.alert_seq;
			return;
		}
	}
	if (model.alert_count == ALERT_MAX) {
		/* Drop the oldest entry. */
		int oldest = 0;

		for (int i = 1; i < ALERT_MAX; i++) {
			if (model.alerts[i].seq < model.alerts[oldest].seq) {
				oldest = i;
			}
		}
		model.alerts[oldest] = model.alerts[--model.alert_count];
	}
	struct ui_alert *a = &model.alerts[model.alert_count++];

	a->seq = ++model.alert_seq;
	a->level = level;
	a->count = 1;
	a->uptime_s = (uint32_t)(k_uptime_get() / 1000);
	a->epoch = model_now_epoch(&model);
	copy_str(a->source, sizeof(a->source), source);
	copy_str(a->text, sizeof(a->text), text);
}

#ifdef CONFIG_LOG
/* Log backend: every error and warning also becomes an alert. */
static char log_line[ALERT_TEXT_LEN + 32];
static size_t log_line_len;
static struct k_spinlock log_lock;

static int log_line_out(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);
	for (size_t i = 0; i < length && log_line_len < sizeof(log_line) - 1; i++) {
		if (data[i] != '\r' && data[i] != '\n') {
			log_line[log_line_len++] = (char)data[i];
		}
	}
	return (int)length;
}

static uint8_t log_out_buf[64];
LOG_OUTPUT_DEFINE(ui_log_output, log_line_out, log_out_buf, sizeof(log_out_buf));

static void ui_log_process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	uint8_t level = log_msg_get_level(&msg->log);

	ARG_UNUSED(backend);
	if (level != LOG_LEVEL_ERR && level != LOG_LEVEL_WRN) {
		return;
	}
	K_SPINLOCK(&log_lock) {
		log_line_len = 0;
		log_output_msg_process(&ui_log_output, &msg->log, LOG_OUTPUT_FLAG_CRLF_NONE);
		log_line[log_line_len] = '\0';

		/* The line is "source: message"; keep them apart for the list. */
		char source[24] = "";
		const char *text = log_line;
		const char *sep = strstr(log_line, ": ");

		if (sep && (size_t)(sep - log_line) < sizeof(source)) {
			memcpy(source, log_line, sep - log_line);
			source[sep - log_line] = '\0';
			text = sep + 2;
		}
		K_SPINLOCK(&model_lock) {
			add_alert_locked(level == LOG_LEVEL_ERR ? ALERT_ERR : ALERT_WARN, source, text);
		}
	}
	mark(D_ALERTS);
}

static void ui_log_panic(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);
}

static const struct log_backend_api ui_log_api = {
	.process = ui_log_process,
	.panic = ui_log_panic,
};

LOG_BACKEND_DEFINE(ui_log_backend, ui_log_api, true);
#endif /* CONFIG_LOG */

/* ========== Formatting ========== */

static uint32_t model_now_epoch(const struct ui_model *m)
{
	if (m->epoch < EPOCH_VALID) {
		return 0;
	}
	return m->epoch + (uint32_t)((k_uptime_get() - m->epoch_at_ms) / 1000);
}

static void fmt_hm(char *out, size_t len, uint32_t epoch, int8_t tz)
{
	if (epoch < EPOCH_VALID) {
		copy_str(out, len, "");
		return;
	}
	uint32_t local = epoch + (int32_t)tz * 3600;

	snprintf(out, len, "%02u:%02u", (unsigned)((local / 3600) % 24), (unsigned)((local / 60) % 60));
}

static void fmt_hops(char *out, size_t len, uint8_t path_len)
{
	if (path_len == OUT_PATH_UNKNOWN) {
		copy_str(out, len, "flood");
	} else if ((path_len & 63) == 0) {
		copy_str(out, len, "direct");
	} else {
		snprintf(out, len, "%u hop%s", path_len & 63, (path_len & 63) == 1 ? "" : "s");
	}
}

static void fmt_age(char *out, size_t len, uint32_t age_s)
{
	if (age_s < 60) {
		snprintf(out, len, "heard %us ago", (unsigned)age_s);
	} else if (age_s < 3600) {
		snprintf(out, len, "heard %um ago", (unsigned)(age_s / 60));
	} else if (age_s < 86400) {
		snprintf(out, len, "heard %uh ago", (unsigned)(age_s / 3600));
	} else {
		snprintf(out, len, "heard %ud ago", (unsigned)(age_s / 86400));
	}
}

static void set_string(lv_subject_t *subject, const char *value)
{
	if (strcmp(lv_subject_get_string(subject), value) != 0) {
		lv_subject_set_string(subject, value);
	}
}

static void set_int(lv_subject_t *subject, int32_t value)
{
	if (lv_subject_get_int(subject) != value) {
		lv_subject_set_int(subject, value);
	}
}

/* ========== Toasts (main thread) ========== */

static void toast_hide(lv_timer_t *timer)
{
	ARG_UNUSED(timer);
	lv_subject_set_int(mcui_toast_level, 0);
}

static void toast(enum alert_level level, const char *text)
{
	lv_subject_set_string(mcui_toast_text, text);
	lv_subject_set_int(mcui_toast_level, level == ALERT_ERR ? 2 : 1);
	lv_timer_reset(toast_timer);
	lv_timer_resume(toast_timer);
}

/* ========== Conversations ========== */

static struct conv_read *conv_read_get(const char *name, bool create)
{
	struct conv_read *free_slot = NULL;
	struct conv_read *oldest = &conv_reads[0];

	for (int i = 0; i < CONV_MAX; i++) {
		if (conv_reads[i].name[0] == '\0') {
			if (!free_slot) {
				free_slot = &conv_reads[i];
			}
		} else if (strcmp(conv_reads[i].name, name) == 0) {
			return &conv_reads[i];
		} else if (conv_reads[i].read_seq < oldest->read_seq) {
			oldest = &conv_reads[i];
		}
	}
	if (!create) {
		return NULL;
	}
	struct conv_read *slot = free_slot ? free_slot : oldest;

	copy_str(slot->name, sizeof(slot->name), name);
	slot->read_seq = 0;
	return slot;
}

static uint32_t conv_read_seq(const char *name)
{
	struct conv_read *r = conv_read_get(name, false);

	return r ? r->read_seq : 0;
}

/* Messages in the ring, oldest first. */
static int msgs_in_order(const struct ui_model *m, const struct ui_msg **out)
{
	uint32_t count = MIN(m->msg_seq, MSG_MAX);
	uint32_t first = m->msg_seq - count;

	for (uint32_t i = 0; i < count; i++) {
		out[i] = &m->msgs[(first + i) % MSG_MAX];
	}
	return (int)count;
}

static void add_bubble(lv_obj_t *list, const struct ui_msg *msg)
{
	static const char *const status_txt[] = {
		[MSG_SENDING] = "sending...",
		[MSG_DELIVERED] = "delivered",
		[MSG_NOT_DELIVERED] = "not delivered",
		[MSG_HEARD] = "heard by a repeater",
		[MSG_NOT_HEARD] = "not heard",
	};
	char when[8], hops[16], meta[40];

	fmt_hm(when, sizeof(when), msg->epoch, view.tz);
	if (msg->status != MSG_RECEIVED) {
		snprintf(meta, sizeof(meta), "%s%s%s", when, when[0] ? " | " : "", status_txt[msg->status]);
		mcui_msg_out_create(list, msg->text, meta);
		return;
	}
	fmt_hops(hops, sizeof(hops), msg->path_len);
	snprintf(meta, sizeof(meta), "%s%s%s", when, when[0] ? " | " : "", hops);
	mcui_msg_in_create(list, msg->from, msg->text, meta);
}

static void rebuild_chats(void)
{
	lv_obj_t *list = lv_obj_find_by_name(mcui_home, "chat_list");
	const struct ui_msg *order[MSG_MAX];
	int n = msgs_in_order(&view, order);
	const char *done[MSG_MAX];
	int ndone = 0;

	if (!list) {
		return;
	}
	lv_obj_clean(list);

	/* Newest conversation first: walk the ring backwards, one row per conversation. */
	for (int i = n - 1; i >= 0; i--) {
		const struct ui_msg *last = order[i];
		bool seen = false;

		for (int d = 0; d < ndone; d++) {
			if (strcmp(done[d], last->conv) == 0) {
				seen = true;
				break;
			}
		}
		if (seen) {
			continue;
		}
		done[ndone++] = last->conv;

		uint32_t read_seq = conv_read_seq(last->conv);
		unsigned unread = 0;

		for (int j = 0; j < n; j++) {
			if (order[j]->seq > read_seq && order[j]->status == MSG_RECEIVED &&
			    strcmp(order[j]->conv, last->conv) == 0) {
				unread++;
			}
		}

		char letter[2] = {last->channel ? '#' : last->conv[0], '\0'};
		char when[8], path[32], preview[TEXT_LEN + NAME_LEN + 2], unread_txt[8];

		if (letter[0] >= 'a' && letter[0] <= 'z') {
			letter[0] = (char)(letter[0] - 'a' + 'A');
		}
		fmt_hm(when, sizeof(when), last->epoch, view.tz);
		if (last->status != MSG_RECEIVED) {
			copy_str(path, sizeof(path), last->channel ? "Channel" : "Direct");
			snprintf(preview, sizeof(preview), "You: %s", last->text);
		} else if (last->channel) {
			copy_str(path, sizeof(path), "Channel");
			snprintf(preview, sizeof(preview), "%s: %s", last->from, last->text);
		} else {
			char hops[16];

			fmt_hops(hops, sizeof(hops), last->path_len);
			snprintf(path, sizeof(path), "Direct | %s", hops);
			copy_str(preview, sizeof(preview), last->text);
		}
		snprintf(unread_txt, sizeof(unread_txt), "%u", unread);
		mcui_chat_row_create(list, last->conv, letter,
				     last->channel ? MCUI_KIND_CHANNEL : MCUI_KIND_CHAT,
				     preview, when, path, unread_txt, unread == 0);
	}

	if (ndone == 0) {
		lv_obj_t *empty = lv_label_create(list);

		lv_label_set_text(empty, "No messages yet. New messages show up here.");
		lv_obj_set_width(empty, lv_pct(100));
		lv_obj_add_style(empty, &mcui_style_text_muted, 0);
	}
}

/* Redraw the open conversation; outcome changes restyle earlier bubbles. */
static void conv_refresh(void)
{
	lv_obj_t *list;
	const struct ui_msg *order[MSG_MAX];
	int n;

	if (!conv_screen) {
		return;
	}
	list = lv_obj_find_by_name(conv_screen, "msg_list");
	if (!list) {
		return;
	}
	lv_obj_clean(list);
	n = msgs_in_order(&view, order);
	for (int i = 0; i < n; i++) {
		if (strcmp(order[i]->conv, conv_name) == 0) {
			add_bubble(list, order[i]);
			conv_shown_seq = order[i]->seq;
		}
	}
	conv_read_get(conv_name, true)->read_seq = view.msg_seq;
	lv_obj_update_layout(list);
	lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void conv_screen_deleted(lv_event_t *e)
{
	ARG_UNUSED(e);
	conv_screen = NULL;
	/* The rows' unread badges changed while the conversation was open. */
	atomic_or(&dirty, D_MSGS);
}

void ui_on_conversation_open(lv_event_t *e)
{
	lv_obj_t *scr = lv_event_get_current_target_obj(e);
	lv_obj_t *list = lv_obj_find_by_name(scr, "msg_list");
	lv_obj_t *kbd = lv_obj_find_by_name(scr, "msg_kbd");
	lv_obj_t *ta = lv_obj_find_by_name(scr, "msg_input");

	if (conv_screen != scr) {
		conv_screen = scr;
		lv_obj_add_event_cb(scr, conv_screen_deleted, LV_EVENT_DELETE, NULL);
	}
	copy_str(conv_name, sizeof(conv_name), lv_subject_get_string(mcui_chat_title));
	conv_shown_seq = 0;
	conv_is_channel = false;
	for (int i = 0; i < MSG_MAX; i++) {
		if (view.msgs[i].seq && strcmp(view.msgs[i].conv, conv_name) == 0) {
			conv_is_channel = view.msgs[i].channel;
			break;
		}
	}
	if (ta) {
		lv_textarea_set_max_length(ta, TEXT_LEN - 1);
	}
	if (kbd && ta) {
		lv_keyboard_set_textarea(kbd, ta);
	}
	ARG_UNUSED(list);
	conv_refresh();
}

static bool radio_usable(const char *what);
static void add_outgoing(const char *conv, const char *text, uint32_t ts, int out_id, bool channel);

void ui_on_send(lv_event_t *e)
{
	lv_obj_t *ta = conv_screen ? lv_obj_find_by_name(conv_screen, "msg_input") : NULL;
	const char *text = ta ? lv_textarea_get_text(ta) : NULL;
	enum ui_send_result r;
	uint32_t ts = 0;
	int id = ++out_id_next;

	ARG_UNUSED(e);
	if (!text || text[0] == '\0' || !radio_usable("Message")) {
		return;
	}
	if (conv_is_channel) {
		r = ui_outbox_send_channel_to(conv_name, text, id, &ts);
	} else {
		r = ui_outbox_send_dm_to(conv_name, text, &ts);
	}
	switch (r) {
	case UI_SEND_OK:
		add_outgoing(conv_name, text, ts, id, conv_is_channel);
		lv_textarea_set_text(ta, "");
		break;
	case UI_SEND_QUEUE_FULL:
		toast(ALERT_ERR, "Not sent: still waiting on earlier messages");
		break;
	case UI_SEND_NO_RECIPIENT:
		toast(ALERT_ERR, conv_is_channel ? "Not sent: this channel isn't set up on the node"
						 : "Not sent: no contact with this name on the node");
		break;
	default:
		toast(ALERT_ERR, "Not sent: the mesh refused the message");
		break;
	}
}

/* ========== Advert buttons ========== */

static void set_last_advert(const char *kind)
{
	char when[8], txt[32];

	fmt_hm(when, sizeof(when), model_now_epoch(&view), view.tz);
	snprintf(txt, sizeof(txt), "%s%s%s", kind, when[0] ? " at " : "", when);
	set_string(mcui_last_advert, txt);
}

static bool radio_usable(const char *what)
{
	char txt[80];

	if (view.radio_known && !view.radio_ready) {
		snprintf(txt, sizeof(txt), "%s not sent: the radio is not available.", what);
		toast(ALERT_ERR, txt);
		return false;
	}
	return true;
}

void ui_on_advert_zerohop(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (!radio_usable("Advert")) {
		set_string(mcui_last_advert, "not sent, radio off");
		return;
	}
	mesh_send_zerohop_advert();
	set_last_advert("zero-hop");
	toast(ALERT_NONE, "Zero-hop advert queued");
}

void ui_on_advert_flood(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (!radio_usable("Advert")) {
		set_string(mcui_last_advert, "not sent, radio off");
		return;
	}
	mesh_send_flood_advert();
	set_last_advert("flood");
	toast(ALERT_NONE, "Flood advert queued");
}

/* ========== Alerts screen ========== */

static void alerts_screen_deleted(lv_event_t *e)
{
	ARG_UNUSED(e);
	alerts_screen = NULL;
}

static void fill_alerts(void)
{
	lv_obj_t *list = alerts_screen ? lv_obj_find_by_name(alerts_screen, "alert_list") : NULL;
	const struct ui_alert *order[ALERT_MAX];
	int n = view.alert_count;

	if (!list) {
		return;
	}
	lv_obj_clean(list);
	for (int i = 0; i < n; i++) {
		order[i] = &view.alerts[i];
	}
	/* Newest first. */
	for (int i = 1; i < n; i++) {
		for (int j = i; j > 0 && order[j]->seq > order[j - 1]->seq; j--) {
			const struct ui_alert *t = order[j];

			order[j] = order[j - 1];
			order[j - 1] = t;
		}
	}
	for (int i = 0; i < n; i++) {
		const struct ui_alert *a = order[i];
		char when[8], meta[32];

		fmt_hm(when, sizeof(when), a->epoch, view.tz);
		if (!when[0]) {
			snprintf(when, sizeof(when), "+%us", (unsigned)MIN(a->uptime_s, 99999u));
		}
		if (a->count > 1) {
			snprintf(meta, sizeof(meta), "%s  x%u", when, a->count);
		} else {
			copy_str(meta, sizeof(meta), when);
		}
		mcui_alert_row_create(list, a->level == ALERT_ERR ? "ERROR" : "WARNING",
				      a->level == ALERT_ERR ? MCUI_DANGER : MCUI_WARN,
				      a->source[0] ? a->source : "system", a->text, meta);
	}
	if (n == 0) {
		lv_obj_t *empty = lv_label_create(list);

		lv_label_set_text(empty, "No problems reported.");
		lv_obj_add_style(empty, &mcui_style_text_muted, 0);
	}
}

static void mark_alerts_seen(void)
{
	K_SPINLOCK(&model_lock) {
		model.alert_seen_seq = model.alert_seq;
	}
	mark(D_ALERTS);
}

void ui_on_alerts_open(lv_event_t *e)
{
	lv_obj_t *scr = lv_event_get_current_target_obj(e);

	if (alerts_screen != scr) {
		alerts_screen = scr;
		lv_obj_add_event_cb(scr, alerts_screen_deleted, LV_EVENT_DELETE, NULL);
	}
	fill_alerts();
	mark_alerts_seen();
}

void ui_on_alerts_clear(lv_event_t *e)
{
	ARG_UNUSED(e);
	K_SPINLOCK(&model_lock) {
		model.alert_count = 0;
		model.alert_seen_seq = model.alert_seq;
	}
	view.alert_count = 0;
	fill_alerts();
	mark(D_ALERTS | D_BANNER);
}

/* ========== Settings: subject -> mesh ========== */

static void settings_observer(lv_observer_t *observer, lv_subject_t *subject)
{
	int32_t v = lv_subject_get_int(subject);

	ARG_UNUSED(observer);
	if (applying || !initialized) {
		return;
	}
	if (subject == mcui_gps_on) {
		if (v && !view.gps_available) {
			toast(ALERT_ERR, "GPS not turned on: no GPS was found on this device.");
			lv_subject_set_int(subject, 0);
			return;
		}
		mesh_gps_set_enabled(v != 0);
	} else if (subject == mcui_ble_on) {
		mesh_ble_set_enabled(v != 0);
	} else if (subject == mcui_buzzer_on) {
		mesh_set_buzzer_mode(v ? 1 : 0);
	} else if (subject == mcui_brightness) {
		mesh_save_brightness((uint8_t)(v * 255 / 100));
	} else if (subject == mcui_screen_timeout) {
		static const uint16_t secs[] = {15, 30, 60, 300, 0};

		if (v >= 0 && v < (int32_t)ARRAY_SIZE(secs)) {
			mesh_save_screen_off_secs(secs[v]);
		}
	}
}

/* ========== Model -> subjects (main thread) ========== */

static void apply_model(uint32_t bits)
{
	char buf[64];

	applying = true;

	if (bits & D_NODE) {
		set_string(mcui_node_name, view.node_name[0] ? view.node_name : "ZephCore");
	}
	if (bits & D_LINK) {
		const char *how = "no app connected";

		if (view.links & UI_LINK_BLE) {
			how = "app on Bluetooth";
		} else if (view.links & UI_LINK_TCP) {
			how = "app on TCP";
		} else if (view.links & UI_LINK_USB) {
			how = "app on USB";
		}
		snprintf(buf, sizeof(buf), "Companion | %s", how);
		set_string(mcui_node_id, buf);
	}
	if (bits & D_CLOCK) {
		fmt_hm(buf, sizeof(buf), model_now_epoch(&view), view.tz);
		set_string(mcui_clock, buf[0] ? buf : "--:--");
	}
	if (bits & D_BATT) {
		set_int(mcui_battery_pct, view.batt_pct);
	}
	if (bits & D_RADIO) {
		snprintf(buf, sizeof(buf), "%u.%03u MHz", (unsigned)(view.freq_hz / 1000000),
			 (unsigned)((view.freq_hz / 1000) % 1000));
		set_string(mcui_radio_freq, buf);
		snprintf(buf, sizeof(buf), "%u.%u kHz", view.bw_x10 / 10, view.bw_x10 % 10);
		set_string(mcui_radio_bw, buf);
		snprintf(buf, sizeof(buf), "SF%u", view.sf);
		set_string(mcui_radio_sf, buf);
		snprintf(buf, sizeof(buf), "4/%u", view.cr);
		set_string(mcui_radio_cr, buf);
		set_int(mcui_tx_power, view.tx_power);
	}
	if (bits & (D_RADIO | D_RECENT)) {
		set_int(mcui_last_rssi, view.recent_count ? view.recent[0].rssi : view.noise_floor);
	}
	if (bits & D_SETTINGS) {
		set_int(mcui_gps_on, view.gps_enabled);
		set_int(mcui_ble_on, view.ble_enabled);
		set_int(mcui_buzzer_on, view.buzzer_mode != 0);
	}
	if (bits & D_COUNT) {
		set_int(mcui_unread_total, view.msg_count);
	}
	if (bits & D_RECENT) {
		lv_obj_t *list = lv_obj_find_by_name(mcui_home, "contact_list");

		if (view.recent_count) {
			snprintf(buf, sizeof(buf), "%u recently heard", view.recent_count);
		} else {
			copy_str(buf, sizeof(buf), "No nodes heard yet");
		}
		set_string(mcui_contacts_summary, buf);
		if (list) {
			lv_obj_clean(list);
			for (int i = 0; i < view.recent_count; i++) {
				const struct ui_recent *r = &view.recent[i];
				char letter[2] = {r->name[0] ? r->name[0] : '?', '\0'};
				char heard[24], rssi[16];

				fmt_age(heard, sizeof(heard), r->age_s);
				snprintf(rssi, sizeof(rssi), "%d dBm", r->rssi);
				mcui_contact_row_create(list, r->name, letter, "HEARD", MCUI_KIND_CHAT,
							heard, rssi, "");
			}
		}
	}
	if (bits & D_MSGS) {
		rebuild_chats();
		conv_refresh();
	}
	if (bits & D_ALERTS) {
		int unseen = 0;
		int level = ALERT_NONE;

		for (int i = 0; i < view.alert_count; i++) {
			if (view.alerts[i].seq > view.alert_seen_seq) {
				unseen++;
				level = MAX(level, view.alerts[i].level);
			}
		}
		set_int(mcui_alert_count, unseen);
		set_int(mcui_alert_level, level);
		fill_alerts();
	}
	if (bits & D_BANNER) {
		if (view.radio_known && !view.radio_ready) {
			set_string(mcui_banner_text, "Radio not available: can't send or receive");
			set_int(mcui_banner_level, ALERT_ERR);
		} else {
			set_int(mcui_banner_level, ALERT_NONE);
		}
	}

	applying = false;
}

static void lvgl_timer_expired(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	wake();
}

static void input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(evt);
	ARG_UNUSED(user_data);
	atomic_set(&input_seen, 1);
	wake();
}
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

#ifdef CONFIG_ZEPHCORE_UI_LVGL_DEMO_DATA
static void demo_fill(void)
{
	ui_add_recent("Giulia", -104, 3 * 3600);
	ui_add_recent("IT Mesh Room", -97, 3600);
	ui_add_recent("Roma-Nord", -101, 22 * 60);
	ui_add_recent("Bob", -95, 15 * 60);
	ui_add_recent("Alice", -88, 120);
	ui_add_recent("Monte Cavo", -79, 60);
	ui_notify_contact_msg(0, "Bob", "ok, sending a flood advert now", 1);
	ui_notify_channel_msg("#roma", "Luca: Monte Cavo repeater is back up", 0, 2, 2);
	ui_notify_contact_msg(1, "Alice", "Hi! Are you near the Monte Cavo repeater?", 3);
	ui_notify_contact_msg(1, "Alice", "See you at the field day!", 4);
	ui_notify_channel_msg("Public", "Marco: anyone on 869.525 tonight?", 0, OUT_PATH_UNKNOWN, 5);
}
#endif

static void outbox_dm_done(uint32_t timestamp, const char *recipient_name, bool delivered);
static void outbox_channel_done(int cookie, uint32_t timestamp, bool heard);

void ui_lvgl_register(void (*signal)(void))
{
	static const struct ui_outbox_hooks outbox_hooks = {
		.wake = wake,
		.dm_done = outbox_dm_done,
		.channel_done = outbox_channel_done,
	};

	signal_fn = signal;
	ui_outbox_set_hooks(&outbox_hooks);
#ifdef CONFIG_ZEPHCORE_UI_LVGL_DEMO_DATA
	demo_fill();
#endif
	atomic_set(&input_seen, 1);
	wake();
}

void ui_lvgl_run(void)
{
	uint32_t bits;
	int64_t now = k_uptime_get();
	lv_indev_t *indev;

	if (!initialized) {
		return;
	}

	ui_outbox_process();

	if (atomic_clear(&input_seen)) {
		last_input_ms = now;
		for (indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
			lv_timer_resume(lv_indev_get_read_timer(indev));
		}
	}

	bits = (uint32_t)atomic_clear(&dirty);
	if (bits) {
		K_SPINLOCK(&model_lock) {
			view = model;
		}
		apply_model(bits);
	}

	uint32_t next = lv_timer_handler();

	/* Displays start blanked; show the panel once the first frame is drawn. */
	if (!unblanked) {
		const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

		if (device_is_ready(disp)) {
			display_blanking_off(disp);
		}
		unblanked = true;
	}

	if (now - last_input_ms > INPUT_IDLE_MS) {
		for (indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev)) {
			if (lv_indev_get_state(indev) == LV_INDEV_STATE_RELEASED) {
				lv_timer_pause(lv_indev_get_read_timer(indev));
			}
		}
	}

	if (next == LV_NO_TIMER_READY) {
		k_timer_stop(&lvgl_timer);
	} else {
		k_timer_start(&lvgl_timer, K_MSEC(MAX(next, 1U)), K_NO_WAIT);
	}
}

/* ========== ui_task.h ========== */

int ui_init(void)
{
	mcui_init_gen();

	/* The XML carries sample rows for the previewer; start empty. */
	lv_obj_t *chats = lv_obj_find_by_name(mcui_home, "chat_list");
	lv_obj_t *contacts = lv_obj_find_by_name(mcui_home, "contact_list");

	if (chats) {
		lv_obj_clean(chats);
	}
	if (contacts) {
		lv_obj_clean(contacts);
	}

	lv_subject_t *settings[] = {mcui_gps_on, mcui_ble_on, mcui_buzzer_on, mcui_brightness,
				    mcui_screen_timeout};

	for (size_t i = 0; i < ARRAY_SIZE(settings); i++) {
		lv_subject_add_observer(settings[i], settings_observer, NULL);
	}

	/* globals.xml carries preview values for the problem subjects. */
	lv_subject_set_int(mcui_alert_count, 0);
	lv_subject_set_int(mcui_banner_level, 0);
	lv_subject_set_int(mcui_toast_level, 0);
	mcui_toast_create(lv_layer_top());
	toast_timer = lv_timer_create(toast_hide, TOAST_MS, NULL);
	lv_timer_set_auto_delete(toast_timer, false);
	lv_timer_set_repeat_count(toast_timer, -1);
	lv_timer_pause(toast_timer);

	k_timer_init(&lvgl_timer, lvgl_timer_expired, NULL);
	lv_screen_load(mcui_home);

#ifdef CONFIG_ZEPHCORE_UI_BUZZER
	buzzer_init();
#endif
	ui_led_heartbeat_init();

	initialized = true;
	atomic_set(&dirty, D_NODE | D_CLOCK | D_BATT | D_RADIO | D_LINK | D_SETTINGS | D_RECENT |
			   D_MSGS | D_COUNT | D_ALERTS | D_BANNER);
	LOG_INF("LVGL UI initialized");
	return 0;
}

static void add_msg(const char *conv, const char *from, const char *text, uint32_t epoch,
		    uint8_t path_len, bool channel)
{
	K_SPINLOCK(&model_lock) {
		struct ui_msg *m = &model.msgs[model.msg_seq % MSG_MAX];

		m->seq = ++model.msg_seq;
		m->epoch = epoch ? epoch : model_now_epoch(&model);
		m->out_id = 0;
		m->status = MSG_RECEIVED;
		m->path_len = path_len;
		m->channel = channel;
		copy_str(m->conv, sizeof(m->conv), conv);
		copy_str(m->from, sizeof(m->from), from);
		copy_str(m->text, sizeof(m->text), text);
	}
	mark(D_MSGS);
}

static void add_outgoing(const char *conv, const char *text, uint32_t ts, int out_id, bool channel)
{
	K_SPINLOCK(&model_lock) {
		struct ui_msg *m = &model.msgs[model.msg_seq % MSG_MAX];

		m->seq = ++model.msg_seq;
		m->epoch = ts;
		m->out_id = (uint32_t)out_id;
		m->path_len = 0;
		m->status = MSG_SENDING;
		m->channel = channel;
		copy_str(m->conv, sizeof(m->conv), conv);
		copy_str(m->from, sizeof(m->from), "You");
		copy_str(m->text, sizeof(m->text), text);
	}
	mark(D_MSGS);
}

static void set_outcome(bool channel, int cookie, const char *conv, uint32_t ts, uint8_t status)
{
	K_SPINLOCK(&model_lock) {
		for (int i = 0; i < MSG_MAX; i++) {
			struct ui_msg *m = &model.msgs[i];

			if (m->status != MSG_SENDING || m->channel != channel || m->epoch != ts) {
				continue;
			}
			if (channel ? m->out_id == (uint32_t)cookie : strcmp(m->conv, conv) == 0) {
				m->status = status;
				break;
			}
		}
	}
	mark(D_MSGS);
}

static void outbox_dm_done(uint32_t timestamp, const char *recipient_name, bool delivered)
{
	set_outcome(false, 0, recipient_name, timestamp, delivered ? MSG_DELIVERED : MSG_NOT_DELIVERED);
}

static void outbox_channel_done(int cookie, uint32_t timestamp, bool heard)
{
	set_outcome(true, cookie, NULL, timestamp, heard ? MSG_HEARD : MSG_NOT_HEARD);
}

void ui_notify_contact_msg(uint8_t path_len, const char *from_name, const char *text,
			   uint16_t msg_count)
{
	add_msg(from_name, from_name, text, 0, path_len, false);
	ui_set_msg_count(msg_count);
	ui_notify(UI_EVENT_CONTACT_MSG);
}

void ui_notify_channel_msg(const char *channel_name, const char *text, uint32_t ts,
			   uint8_t path_len, uint16_t msg_count)
{
	/* Channel text is "sender: message". */
	char from[NAME_LEN] = "";
	const char *colon = text ? strstr(text, ": ") : NULL;
	const char *body = text;

	if (colon && (size_t)(colon - text) < sizeof(from)) {
		memcpy(from, text, colon - text);
		from[colon - text] = '\0';
		body = colon + 2;
	}
	add_msg(channel_name, from, body, ts, path_len, true);
	ui_set_msg_count(msg_count);
	ui_notify(UI_EVENT_CHANNEL_MSG);
}

void ui_notify_packet_sent(void)
{
}

void ui_notify(enum ui_event event)
{
	bool app = false;
	bool is_msg = event == UI_EVENT_CONTACT_MSG || event == UI_EVENT_CHANNEL_MSG ||
		      event == UI_EVENT_ROOM_MSG;

	K_SPINLOCK(&model_lock) {
		if (event == UI_EVENT_APP_CONNECTED) {
			model.app_connected = true;
		} else if (event == UI_EVENT_APP_DISCONNECTED) {
			model.app_connected = false;
		}
		app = model.app_connected;
	}

#ifdef CONFIG_ZEPHCORE_UI_BUZZER
	if (!app && event == UI_EVENT_CONTACT_MSG) {
		buzzer_play(MELODY_MSG_CONTACT);
	} else if (!app && (event == UI_EVENT_CHANNEL_MSG || event == UI_EVENT_ROOM_MSG)) {
		buzzer_play(MELODY_MSG_CHANNEL);
	} else if (event == UI_EVENT_ACK) {
		buzzer_play(MELODY_ACK);
	}
#endif
	if (is_msg && !app) {
		ui_led_flash_msg();
	}
	mark(D_LINK);
}

void ui_set_msg_count(uint16_t count)
{
	K_SPINLOCK(&model_lock) {
		model.msg_count = count;
	}
	mark(D_COUNT);
}

void ui_set_radio_params(uint32_t freq_hz, uint8_t sf, uint16_t bw_khz_x10, uint8_t cr,
			 int8_t tx_power, int16_t noise_floor)
{
	K_SPINLOCK(&model_lock) {
		model.freq_hz = freq_hz;
		model.sf = sf;
		model.bw_x10 = bw_khz_x10;
		model.cr = cr;
		model.tx_power = tx_power;
		model.noise_floor = noise_floor;
	}
	mark(D_RADIO);
}

void ui_set_radio_runtime(uint8_t sync_word, uint16_t preamble_len, bool rx_duty_cycle,
			  bool radio_ready, bool in_rx, bool tx_active)
{
	ARG_UNUSED(sync_word);
	ARG_UNUSED(preamble_len);
	ARG_UNUSED(rx_duty_cycle);
	ARG_UNUSED(radio_ready);
	ARG_UNUSED(in_rx);
	ARG_UNUSED(tx_active);
}

void ui_set_radio_available(bool available)
{
	bool changed;

	K_SPINLOCK(&model_lock) {
		changed = !model.radio_known || model.radio_ready != available;
		model.radio_known = true;
		model.radio_ready = available;
		if (changed && !available) {
			add_alert_locked(ALERT_ERR, "radio", "No working LoRa radio was found");
		}
	}
	if (changed) {
		mark(D_BANNER | D_ALERTS);
	}
}

void ui_set_radio_stats(uint32_t packets_rx, uint32_t packets_tx, uint32_t packets_err)
{
	ARG_UNUSED(packets_rx);
	ARG_UNUSED(packets_tx);
	ARG_UNUSED(packets_err);
}

void ui_set_gps_data(bool has_fix, uint8_t sats, int32_t lat_mdeg, int32_t lon_mdeg,
		     int32_t alt_mm)
{
	ARG_UNUSED(has_fix);
	ARG_UNUSED(sats);
	ARG_UNUSED(lat_mdeg);
	ARG_UNUSED(lon_mdeg);
	ARG_UNUSED(alt_mm);
}

void ui_set_battery(uint16_t mv, uint8_t pct)
{
	K_SPINLOCK(&model_lock) {
		model.batt_mv = mv;
		model.batt_pct = pct;
	}
	mark(D_BATT);
}

void ui_set_clock(uint32_t epoch)
{
	K_SPINLOCK(&model_lock) {
		model.epoch = epoch;
		model.epoch_at_ms = k_uptime_get();
	}
	mark(D_CLOCK);
}

void ui_set_tz(int8_t hours)
{
	K_SPINLOCK(&model_lock) {
		model.tz = hours;
	}
	mark(D_CLOCK | D_MSGS);
}

void ui_add_recent(const char *name, int16_t rssi, uint32_t age_s)
{
	K_SPINLOCK(&model_lock) {
		uint8_t n = MIN(model.recent_count, RECENT_MAX - 1);

		memmove(&model.recent[1], &model.recent[0], sizeof(model.recent[0]) * n);
		copy_str(model.recent[0].name, sizeof(model.recent[0].name), name);
		model.recent[0].rssi = rssi;
		model.recent[0].age_s = age_s;
		model.recent_count = n + 1;
	}
	mark(D_RECENT);
}

void ui_clear_recent(void)
{
	K_SPINLOCK(&model_lock) {
		model.recent_count = 0;
	}
	mark(D_RECENT);
}

void ui_set_node_name(const char *name)
{
	K_SPINLOCK(&model_lock) {
		copy_str(model.node_name, sizeof(model.node_name), name);
	}
	mark(D_NODE);
}

void ui_set_gps_available(bool available)
{
	K_SPINLOCK(&model_lock) {
		model.gps_available = available;
	}
	mark(D_SETTINGS);
}

void ui_show_low_battery(bool hold)
{
	/* Called on the main thread by app/PowerPolicy.cpp just before shutdown. */
	if (!initialized) {
		return;
	}
	mcui_notice_overlay_create(lv_layer_top(), "Low battery", "Shutting down to protect the battery");
	lv_refr_now(NULL);
	if (hold) {
		k_sleep(K_MSEC(3000));
	}
}

void ui_set_gps_enabled(bool enabled)
{
	K_SPINLOCK(&model_lock) {
		model.gps_enabled = enabled;
	}
	mark(D_SETTINGS);
}

void ui_set_gps_state(uint8_t state, uint32_t last_fix_age_s, uint32_t next_search_s)
{
	ARG_UNUSED(state);
	ARG_UNUSED(last_fix_age_s);
	ARG_UNUSED(next_search_s);
}

void ui_set_ble_enabled(bool enabled)
{
	K_SPINLOCK(&model_lock) {
		model.ble_enabled = enabled;
	}
	mark(D_SETTINGS);
}

void ui_set_link_transports(uint8_t mask)
{
	K_SPINLOCK(&model_lock) {
		model.links = mask;
	}
	mark(D_LINK);
}

void ui_set_wifi(uint8_t state, const char *ssid, const char *ip, uint16_t port)
{
	ARG_UNUSED(state);
	ARG_UNUSED(ssid);
	ARG_UNUSED(ip);
	ARG_UNUSED(port);
}

void ui_set_buzzer_mode(uint8_t mode)
{
	K_SPINLOCK(&model_lock) {
		model.buzzer_mode = mode;
	}
	mark(D_SETTINGS);
}

void ui_set_offgrid_mode(bool enabled)
{
	ARG_UNUSED(enabled);
}
