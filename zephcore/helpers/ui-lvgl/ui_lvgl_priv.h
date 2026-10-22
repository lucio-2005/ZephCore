/*
 * ZephCore - LVGL touch UI, shared between its source files
 * SPDX-License-Identifier: MIT
 *
 * Main thread only.
 */

#ifndef ZEPHCORE_UI_LVGL_PRIV_H
#define ZEPHCORE_UI_LVGL_PRIV_H

#include <stdbool.h>

/* Show a short message over everything; error toasts are red. */
void ui_lvgl_toast(bool error, const char *text);

/* ui_lvgl_settings.c: register observers once the generated UI exists. */
void ui_lvgl_settings_init(void);

/* Reload the settings subjects and menu summaries from NodePrefs. */
void ui_lvgl_settings_load(void);

#endif /* ZEPHCORE_UI_LVGL_PRIV_H */
