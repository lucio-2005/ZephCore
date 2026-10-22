/*
 * ZephCore - LVGL touch UI: joystick and keypad navigation
 * SPDX-License-Identifier: MIT
 *
 * Takes the key events every joystick or keypad board already produces
 * (gpio-keys plus the longpress / multi-tap filters, through
 * zephcore_input_map_code() for upside-down mounts) and drives the UI with
 * them, with the joystick UI's conventions:
 *
 *   up / down       previous / next item; held: first / last item
 *   centre          select; held: home screen
 *   left / right    home screen: previous / next tab
 *   left            elsewhere: back (or adjust the slider, keyboard, text field, map)
 *   right           elsewhere: nothing (or adjust the same)
 *   back / user btn back; 2-5 taps: LEDs, buzzer, GPS, flood advert
 *   typed keys      into the text field (keypad boards)
 *
 * The items that can be focused are the active screen's buttons, rows,
 * switches, sliders, dropdowns, text areas and keyboards (or those of a panel
 * open on the top layer), in screen order. The group is rebuilt whenever that
 * set changes, which is checked on every run once a key has been pressed:
 * touch-only use costs nothing.
 *
 * Input thread: ui_lvgl_keys_input(). Everything else: main thread.
 */

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/atomic.h>
#include <lvgl.h>
#include <string.h>

#include "ui_task.h"
#include "ui_lvgl_priv.h"
#include "mcui_gen.h"
#include <helpers/input/zephcore_input_ascii.h>

#define HOLD_MS		500	/* as the joystick UI's long press */
#define QUEUE_LEN	16
#define FOCUS_MAX	160

struct raw_key {
	uint16_t code;
	bool held;
};

/* Input thread -> main thread */
static struct raw_key queue[QUEUE_LEN];
static uint8_t q_head, q_count;
static struct k_spinlock q_lock;
static atomic_t keys_seen;
static int64_t press_ms[3];	/* ENTER, UP, DOWN */

static lv_group_t *group;
static lv_indev_t *indev;
static uint32_t pressed_key;	/* reported as released on the next read */
static lv_obj_t *focus_list[FOCUS_MAX];
static int focus_n;
static lv_obj_t *want_focus;	/* focus this once it is in the group */
static lv_obj_t *last_root;	/* the screen or panel of the last sync */
static int last_idx;		/* the focused item's place in focus_list */
static int home_idx = -1;	/* last_idx when home was left */
static bool focus_lost;		/* the focused item was deleted ... */
static int lost_idx;		/* ... from this place */
static bool cursor_shown;

/* ========== Input thread ========== */

static void push(uint16_t code, bool held)
{
	K_SPINLOCK(&q_lock) {
		if (q_count < QUEUE_LEN) {
			queue[(q_head + q_count) % QUEUE_LEN] = (struct raw_key){code, held};
			q_count++;
		}
	}
	atomic_set(&keys_seen, 1);
}

static bool pop(struct raw_key *out)
{
	bool ok = false;

	K_SPINLOCK(&q_lock) {
		if (q_count) {
			*out = queue[q_head];
			q_head = (q_head + 1) % QUEUE_LEN;
			q_count--;
			ok = true;
		}
	}
	return ok;
}

void ui_lvgl_keys_input(const struct input_event *evt)
{
	uint16_t code;
	int slot = -1;

	if (evt->type != INPUT_EV_KEY) {
		return;
	}
	code = zephcore_input_map_code(evt->code);
	switch (code) {
	case INPUT_KEY_ENTER: slot = 0; break;
	case INPUT_KEY_UP: slot = 1; break;
	case INPUT_KEY_DOWN: slot = 2; break;
	default: break;
	}
	/* These three tell a hold from a tap, so they act on release. */
	if (slot >= 0) {
		if (evt->value) {
			press_ms[slot] = k_uptime_get();
		} else {
			push(code, k_uptime_get() - press_ms[slot] >= HOLD_MS);
		}
		return;
	}
	if (evt->value) {
		push(code, false);
	}
}

/* ========== Focus ========== */

static lv_obj_t *home_tabs(void);

static bool is_named(lv_obj_t *o, const char *name)
{
	const char *n = lv_obj_get_name(o);

	return n && strcmp(n, name) == 0;
}

static bool is_widget(lv_obj_t *o)
{
	return lv_obj_check_type(o, &lv_button_class) || lv_obj_check_type(o, &lv_switch_class) ||
	       lv_obj_check_type(o, &lv_slider_class) || lv_obj_check_type(o, &lv_dropdown_class) ||
	       lv_obj_check_type(o, &lv_textarea_class) || lv_obj_check_type(o, &lv_keyboard_class);
}

static bool focusable(lv_obj_t *o)
{
	return lv_obj_is_clickable(o) &&
	       (is_widget(o) || is_named(o, "map_area"));
}

static void collect(lv_obj_t *o, lv_obj_t **out, int *n)
{
	if (lv_obj_is_hidden(o)) {
		return;
	}
	if (*n < FOCUS_MAX && focusable(o)) {
		out[(*n)++] = o;
	}
	if (lv_obj_check_type(o, &lv_tabview_class)) {
		/* Only the tab on show, then the tab buttons. */
		lv_obj_t *tab = lv_obj_get_child(lv_tabview_get_content(o), (int32_t)lv_tabview_get_tab_active(o));

		if (tab) {
			collect(tab, out, n);
		}
		collect(lv_tabview_get_tab_bar(o), out, n);
		return;
	}
	if (is_widget(o) && !lv_obj_check_type(o, &lv_button_class)) {
		return;
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
		collect(lv_obj_get_child(o, (int32_t)i), out, n);
	}
}

/* A panel open over the screen (edit panel, console, remote settings). */
static lv_obj_t *modal_panel(void)
{
	lv_obj_t *top = lv_layer_top();

	for (int32_t i = (int32_t)lv_obj_get_child_count(top) - 1; i >= 0; i--) {
		lv_obj_t *c = lv_obj_get_child(top, i);

		if (!lv_obj_is_hidden(c) && lv_obj_find_by_name(c, "panel_close")) {
			return c;
		}
	}
	return NULL;
}

static lv_obj_t *focus_root(void)
{
	lv_obj_t *panel = modal_panel();

	return panel ? panel : lv_screen_active();
}

static lv_obj_t *find_class(lv_obj_t *root, const lv_obj_class_t *cls)
{
	lv_obj_t *found = NULL;

	for (int i = 0; i < focus_n && !found; i++) {
		if (lv_obj_check_type(focus_list[i], cls) && lv_obj_get_screen(focus_list[i]) == lv_obj_get_screen(root)) {
			found = focus_list[i];
		}
	}
	return found;
}

/* Focus with the cursor shown: LVGL only marks FOCUS_KEY when the focus
 * moves during a keypad read. */
static void focus_on(lv_obj_t *o)
{
	lv_group_focus_obj(o);
	if (cursor_shown || atomic_get(&keys_seen)) {
		lv_obj_add_state(o, LV_STATE_FOCUS_KEY);
	}
}

static bool is_tab_button(lv_obj_t *o)
{
	lv_obj_t *tv = home_tabs();

	return tv && lv_obj_get_parent(o) == lv_tabview_get_tab_bar(tv);
}

static bool is_inside(lv_obj_t *o, lv_obj_t *ancestor)
{
	for (; o; o = lv_obj_get_parent(o)) {
		if (o == ancestor) {
			return true;
		}
	}
	return false;
}

/* Where a screen starts: the active tab's first item, a text field, or the
 * first item below the header. */
static lv_obj_t *start_focus(lv_obj_t *root)
{
	lv_obj_t *tv = home_tabs();

	if (root == mcui_home && tv) {
		lv_obj_t *tab = lv_obj_get_child(lv_tabview_get_content(tv), (int32_t)lv_tabview_get_tab_active(tv));

		for (int i = 0; i < focus_n; i++) {
			if (is_inside(focus_list[i], tab)) {
				return focus_list[i];
			}
		}
	}
	for (int i = 0; i < focus_n; i++) {
		if (lv_obj_check_type(focus_list[i], &lv_textarea_class)) {
			return focus_list[i];
		}
	}
	/* Skip the header: the row that holds the Back button. */
	lv_obj_t *back = lv_obj_find_by_name(root, "nav_back");
	lv_obj_t *header = back ? lv_obj_get_parent(back) : NULL;

	for (int i = 0; i < focus_n; i++) {
		if (!header || !is_inside(focus_list[i], header)) {
			return focus_list[i];
		}
	}
	return focus_n ? focus_list[0] : NULL;
}

static int index_of(lv_obj_t *o)
{
	for (int i = 0; i < focus_n; i++) {
		if (focus_list[i] == o) {
			return i;
		}
	}
	return -1;
}

static void focused_deleted(lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj(e);

	/* LVGL moves the focus on by itself; put it back at the same place. */
	if (o == lv_group_get_focused(group) && index_of(o) >= 0) {
		lost_idx = index_of(o);
		focus_lost = true;
	}
}

static void focus_sync(void)
{
	static lv_obj_t *now[FOCUS_MAX];
	int n = 0;
	lv_obj_t *f = lv_group_get_focused(group);
	lv_obj_t *root = focus_root();
	lv_obj_t *pick = NULL;

	if (f && lv_obj_check_type(f, &lv_dropdown_class) && lv_dropdown_is_open(f)) {
		return;		/* its list is in use */
	}
	collect(root, now, &n);
	if (root == last_root && n == focus_n && memcmp(now, focus_list, sizeof(now[0]) * (size_t)n) == 0) {
		goto done;
	}
	if (root != last_root && last_root == mcui_home) {
		home_idx = last_idx;
	}
	lv_group_remove_all_objs(group);
	memcpy(focus_list, now, sizeof(now[0]) * (size_t)n);
	focus_n = n;
	for (int i = 0; i < n; i++) {
		/* Once per object: removing first keeps these from stacking. */
		lv_style_t *fs = is_tab_button(now[i]) ? &mcui_style_focus_inner : &mcui_style_focus;

		lv_obj_remove_style(now[i], fs, LV_STATE_FOCUS_KEY);
		lv_obj_add_style(now[i], fs, LV_STATE_FOCUS_KEY);
		lv_obj_remove_event_cb(now[i], focused_deleted);
		lv_obj_add_event_cb(now[i], focused_deleted, LV_EVENT_DELETE, NULL);
		lv_group_add_obj(group, now[i]);
	}
	if (n == 0) {
		goto done;
	}
	if (root == last_root) {
		/* Same screen: the same item, or the one now in its place. */
		pick = (!focus_lost && index_of(f) >= 0) ? f : now[MIN(focus_lost ? lost_idx : last_idx, n - 1)];
	} else if (modal_panel()) {
		/* A panel starts on its keyboard. */
		pick = find_class(root, &lv_keyboard_class);
	} else if (root == mcui_home && home_idx >= 0) {
		pick = now[MIN(home_idx, n - 1)];
	}
	if (!pick) {
		pick = start_focus(root);
	}
	focus_on(pick);
done:
	focus_lost = false;
	if (want_focus && index_of(want_focus) >= 0) {
		focus_on(want_focus);
		want_focus = NULL;
	}
	last_root = root;
	last_idx = MAX(index_of(lv_group_get_focused(group)), 0);
}

/* ========== Actions ========== */

static void click(lv_obj_t *o)
{
	if (o) {
		lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
	}
}

static lv_obj_t *home_tabs(void)
{
	return lv_obj_find_by_name(mcui_home, "home_tabs");
}

static void go_back(void)
{
	lv_obj_t *f = lv_group_get_focused(group);
	lv_obj_t *panel = modal_panel();

	if (panel) {
		click(lv_obj_find_by_name(panel, "panel_close"));
		return;
	}
	/* A message keyboard shown on request is hidden first. */
	if (f && is_named(f, "msg_kbd")) {
		lv_subject_set_int(mcui_show_keyboard, 0);
		want_focus = lv_obj_find_by_name(lv_screen_active(), "msg_input");
		return;
	}
	if (ui_lvgl_map_back()) {
		return;
	}
	lv_obj_t *back = lv_obj_find_by_name(lv_screen_active(), "nav_back");

	if (back) {
		click(back);
	} else if (lv_screen_active() == mcui_home && home_tabs()) {
		lv_tabview_set_active(home_tabs(), 0, LV_ANIM_ON);
	}
}

static void go_home(void)
{
	lv_obj_t *panel = modal_panel();

	if (panel) {
		click(lv_obj_find_by_name(panel, "panel_close"));
	}
	if (lv_screen_active() != mcui_home) {
		lv_screen_load_anim(mcui_home, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
	}
}

static void focus_edge(bool last)
{
	uint32_t n = lv_group_get_obj_count(group);

	if (n) {
		lv_group_focus_obj(lv_group_get_obj_by_index(group, last ? n - 1 : 0));
	}
}

/* Left / right on the home screen: the neighbouring tab. The focus follows to
 * its tab button when it was on the tab bar, else to the tab's first item. */
static void switch_tab(bool on_tab_bar, int dir)
{
	lv_obj_t *tv = home_tabs();
	lv_obj_t *content = lv_tabview_get_content(tv);
	int32_t n = (int32_t)lv_obj_get_child_count(content);
	int32_t i = (int32_t)lv_tabview_get_tab_active(tv) + dir;
	static lv_obj_t *items[FOCUS_MAX];
	int count = 0;

	if (i < 0 || i >= n) {
		return;
	}
	/* Without animation when the focus goes into the tab: focusing an item
	 * mid-scroll scrolls it into view from the old tab, which snaps back. */
	lv_tabview_set_active(tv, (uint32_t)i, on_tab_bar ? LV_ANIM_ON : LV_ANIM_OFF);
	if (!on_tab_bar) {
		collect(lv_obj_get_child(content, i), items, &count);
	}
	want_focus = count ? items[0] : lv_obj_get_child(lv_tabview_get_tab_bar(tv), i);
}

/* Centre on a text field: bring up and focus its keyboard. */
static void open_keyboard(lv_obj_t *ta)
{
	lv_obj_t *kbd = find_class(focus_root(), &lv_keyboard_class);

	if (!kbd) {
		kbd = lv_obj_find_by_name(lv_screen_active(), "msg_kbd");
		if (kbd) {
			lv_subject_set_int(mcui_show_keyboard, 1);
		}
	}
	if (kbd) {
		lv_keyboard_set_textarea(kbd, ta);
		want_focus = kbd;
	}
}

static lv_obj_t *text_target(lv_obj_t *f)
{
	if (f && lv_obj_check_type(f, &lv_textarea_class)) {
		return f;
	}
	return find_class(focus_root(), &lv_textarea_class);
}

/* From a tab button into its tab: the first item going down, the last going up. */
static void focus_tab_content(bool first)
{
	lv_obj_t *tv = home_tabs();
	lv_obj_t *tab = lv_obj_get_child(lv_tabview_get_content(tv), (int32_t)lv_tabview_get_tab_active(tv));
	lv_obj_t *pick = NULL;

	for (int i = 0; i < focus_n; i++) {
		if (is_inside(focus_list[i], tab) && (!pick || !first)) {
			pick = focus_list[i];
		}
	}
	if (pick) {
		focus_on(pick);
	}
}

/* The LVGL key for a joystick or keypad key, or 0 when it was handled here. */
static uint32_t translate(struct raw_key r)
{
	lv_obj_t *f = lv_group_get_focused(group);
	bool kbd = f && lv_obj_check_type(f, &lv_keyboard_class);
	bool map = f && is_named(f, "map_area");
	bool dd_open = f && lv_obj_check_type(f, &lv_dropdown_class) && lv_dropdown_is_open(f);
	bool slider = f && lv_obj_check_type(f, &lv_slider_class);
	bool ta = f && lv_obj_check_type(f, &lv_textarea_class);
	bool tab = f && home_tabs() && lv_obj_get_parent(f) == lv_tabview_get_tab_bar(home_tabs());
	lv_obj_t *t;

	switch (r.code) {
	case INPUT_KEY_UP:
	case INPUT_KEY_DOWN: {
		bool down = r.code == INPUT_KEY_DOWN;

		if (!cursor_shown) {
			/* The first press only shows where the cursor is. */
			return 0;
		}
		if (tab && !r.held) {
			focus_tab_content(down);
			return 0;
		}
		if (r.held) {
			if (map) {
				ui_lvgl_map_zoom(!down);
			} else {
				focus_edge(down);
			}
			return 0;
		}
		if (kbd || map || dd_open) {
			return down ? LV_KEY_DOWN : LV_KEY_UP;
		}
		return down ? LV_KEY_NEXT : LV_KEY_PREV;
	}
	case INPUT_KEY_LEFT:
	case INPUT_KEY_RIGHT: {
		bool right = r.code == INPUT_KEY_RIGHT;

		if (dd_open) {
			return 0;
		}
		/* On the home screen they only change tabs. */
		if (lv_screen_active() == mcui_home && !modal_panel() && home_tabs()) {
			switch_tab(tab, right ? 1 : -1);
			return 0;
		}
		if (kbd || map || slider || ta) {
			return right ? LV_KEY_RIGHT : LV_KEY_LEFT;
		}
		if (!right) {
			go_back();
		}
		return 0;
	}
	case INPUT_KEY_ENTER:
		if (r.held) {
			if (map) {
				ui_lvgl_map_next();
			} else {
				go_home();
			}
			return 0;
		}
		if (ta) {
			open_keyboard(f);
			return 0;
		}
		return LV_KEY_ENTER;
	case INPUT_KEY_BACK:
	case INPUT_KEY_ESC:
	case INPUT_KEY_1:
		if (dd_open) {
			return LV_KEY_ESC;
		}
		go_back();
		return 0;
	case INPUT_KEY_HOME:
	case INPUT_KEY_F2:
		go_home();
		return 0;
	case INPUT_KEY_MENU:
		return LV_KEY_ENTER;
	case INPUT_KEY_PAGEUP:
		return LV_KEY_PREV;
	case INPUT_KEY_PAGEDOWN:
		return LV_KEY_NEXT;
	/* The user button's multi-tap shortcuts, as on the joystick UI */
	case INPUT_KEY_B:
		ui_lvgl_shortcut(UI_SHORTCUT_LEDS);
		return 0;
	case INPUT_KEY_D:
		ui_lvgl_shortcut(UI_SHORTCUT_BUZZER);
		return 0;
	case INPUT_KEY_C:
		ui_lvgl_shortcut(UI_SHORTCUT_GPS);
		return 0;
	case INPUT_KEY_E:
		ui_lvgl_shortcut(UI_SHORTCUT_FLOOD_ADVERT);
		return 0;
	case INPUT_KEY_BACKSPACE:
		t = text_target(f);
		if (t) {
			lv_textarea_delete_char(t);
		}
		return 0;
	default:
		/* Keypad boards type characters straight into the text field. */
		if (ZEPHCORE_INPUT_IS_ASCII(r.code)) {
			t = text_target(f);
			if (t) {
				lv_textarea_add_char(t, (uint32_t)ZEPHCORE_INPUT_TO_ASCII(r.code));
				want_focus = t;
			}
		}
		return 0;
	}
}

static void keypad_read(lv_indev_t *dev, lv_indev_data_t *data)
{
	struct raw_key r = {0};

	ARG_UNUSED(dev);
	if (pressed_key) {
		data->key = pressed_key;
		data->state = LV_INDEV_STATE_RELEASED;
		pressed_key = 0;
		data->continue_reading = q_count > 0;
		return;
	}
	data->state = LV_INDEV_STATE_RELEASED;
	while (pop(&r)) {
		uint32_t key = translate(r);

		cursor_shown = true;
		if (key) {
			data->key = key;
			data->state = LV_INDEV_STATE_PRESSED;
			pressed_key = key;
			data->continue_reading = true;
			return;
		}
	}
}

/* ========== Main loop hooks ========== */

void ui_lvgl_keys_init(void)
{
	group = lv_group_create();
	lv_group_set_wrap(group, true);
	indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
	lv_indev_set_read_cb(indev, keypad_read);
	lv_indev_set_group(indev, group);
}

void ui_lvgl_keys_run(void)
{
	if (atomic_get(&keys_seen)) {
		focus_sync();
	}
}
