/*
 * ZephCore - LVGL touch UI, shared between its source files
 * SPDX-License-Identifier: MIT
 *
 * Main thread only.
 */

#ifndef ZEPHCORE_UI_LVGL_PRIV_H
#define ZEPHCORE_UI_LVGL_PRIV_H

#include <stdbool.h>
#include <stdint.h>
#include <lvgl.h>

/* ui_lvgl.c */

/* Show a short message over everything; error toasts are red. */
void ui_lvgl_toast(bool error, const char *text);

/* Load a screen made in C; it is deleted once another screen is loaded. */
void ui_lvgl_load_new_screen(lv_obj_t *scr);

/* Open the conversation with a contact or channel by name. */
void ui_lvgl_open_chat(const char *name, const char *path);

/* Run LVGL soon (after changing objects outside its handler). */
void ui_lvgl_wake(void);

/* ui_lvgl_edit.c */

struct ui_edit_req {
	const char *title;
	const char *hint;
	const char *initial;
	bool number;            /* number keyboard, digits . and - only */
	bool password;          /* show dots */
	uint8_t max_len;
	/* Applies the text: true closes the panel, false shows *err. */
	bool (*save)(const char *text, const char **err);
};

/* False if a panel is already open. */
bool ui_lvgl_edit_open(const struct ui_edit_req *req);

/* ui_lvgl_settings.c */

/* Register observers once the generated UI exists. */
void ui_lvgl_settings_init(void);

/* Reload the settings subjects and menu summaries from NodePrefs. */
void ui_lvgl_settings_load(void);

void ui_lvgl_settings_edit(const char *key);

/* ui_lvgl_remote.c: the Contacts tab and remote management */

void ui_lvgl_remote_init(void);
void ui_lvgl_remote_edit(const char *key);
/* Rebuild the Contacts tab if the contact table changed (or force). */
void ui_lvgl_contacts_refresh(bool force);
void ui_lvgl_remote_packet_sent(void);
/* A companion opens a chat, any other node the remote screen. */
void ui_lvgl_open_contact(const uint8_t *pub_key);

/* ui_lvgl_map.c */

/* Reload the nodes shown if the map is open. */
void ui_lvgl_map_refresh(void);

#endif /* ZEPHCORE_UI_LVGL_PRIV_H */
