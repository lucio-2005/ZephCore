/*
 * ZephCore - LVGL touch UI
 * SPDX-License-Identifier: MIT
 *
 * Main-thread hooks for the LVGL frontend. The ui_task.h API feeds it from any
 * thread; LVGL itself only ever runs inside ui_lvgl_run() on the main thread.
 */

#ifndef ZEPHCORE_UI_LVGL_H
#define ZEPHCORE_UI_LVGL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Set the function that wakes the main loop to call ui_lvgl_run().
 * It must be safe to call from ISRs and other threads. Call after the main
 * loop's event object is initialised; wakes it once to draw the first frame.
 */
void ui_lvgl_register(void (*signal)(void));

/**
 * Apply pending state, run LVGL, and arm the timer for the next LVGL deadline.
 * Main thread only.
 */
void ui_lvgl_run(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHCORE_UI_LVGL_H */
