/*
 * ZephCore - LVGL touch UI: the edit panel
 * SPDX-License-Identifier: MIT
 *
 * One text or number value at a time, on the top layer, with its own
 * keyboard. The caller's save() checks and applies the text; the panel stays
 * open with an error toast when it refuses. Main thread only.
 */

#include <zephyr/kernel.h>
#include <lvgl.h>
#include <string.h>

#include "ui_lvgl_priv.h"
#include "mcui_gen.h"

static lv_obj_t *panel;
static bool (*panel_save)(const char *text, const char **err);

static void edit_close(void)
{
	if (panel) {
		/* Called from events of the panel's own children. */
		lv_obj_delete_async(panel);
		panel = NULL;
	}
	panel_save = NULL;
}

static void edit_save(void)
{
	const char *err = NULL;

	if (!panel || !panel_save) {
		return;
	}
	lv_obj_t *ta = lv_obj_find_by_name(panel, "edit_input");

	if (!ta || !panel_save(lv_textarea_get_text(ta), &err)) {
		ui_lvgl_toast(true, err ? err : "Not saved");
		return;
	}
	edit_close();
}

void ui_on_edit_save(lv_event_t *e)
{
	ARG_UNUSED(e);
	edit_save();
}

void ui_on_edit_cancel(lv_event_t *e)
{
	ARG_UNUSED(e);
	edit_close();
}

static void edit_kbd_event(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_READY) {
		edit_save();
	} else {
		edit_close();
	}
}

bool ui_lvgl_edit_open(const struct ui_edit_req *req)
{
	if (panel) {
		return false;
	}
	lv_subject_set_string(mcui_edit_title, req->title);
	lv_subject_set_string(mcui_edit_hint, req->hint ? req->hint : "");
	panel = mcui_edit_panel_create(lv_layer_top());
	panel_save = req->save;

	lv_obj_t *ta = lv_obj_find_by_name(panel, "edit_input");
	lv_obj_t *kbd = lv_obj_find_by_name(panel, "edit_kbd");

	if (!ta || !kbd) {
		edit_close();
		return false;
	}
	lv_textarea_set_max_length(ta, req->max_len);
	if (req->number) {
		lv_textarea_set_accepted_chars(ta, "0123456789.-");
		lv_keyboard_set_mode(kbd, LV_KEYBOARD_MODE_NUMBER);
	}
	lv_textarea_set_password_mode(ta, req->password);
	lv_textarea_set_text(ta, req->initial ? req->initial : "");
	lv_keyboard_set_textarea(kbd, ta);
	lv_obj_add_state(ta, LV_STATE_FOCUSED);
	lv_obj_add_event_cb(kbd, edit_kbd_event, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(kbd, edit_kbd_event, LV_EVENT_CANCEL, NULL);
	return true;
}

/* value_row: the key says which module owns the value. */
void ui_on_edit(lv_event_t *e)
{
	const char *key = lv_event_get_user_data(e);

	if (!key || panel) {
		return;
	}
	if (strncmp(key, "r.", 2) == 0) {
		ui_lvgl_remote_edit(key);
	} else {
		ui_lvgl_settings_edit(key);
	}
}
