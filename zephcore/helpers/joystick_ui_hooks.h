/*
 * ZephCore - Joystick UI Hooks (public API)
 * Copyright (c) 2026 ZephCore
 * SPDX-License-Identifier: MIT
 *
 * Mesh layer → joystick UI bridge. Real implementations live in
 * helpers/ui-joystick/joystick_ui_hooks.cpp when CONFIG_ZEPHCORE_UI_DESIGN_JOYSTICK=y.
 * Inline no-ops below keep call sites free of #ifdef when joystick UI is off.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/sys/util.h>
#include <ui_remote_events.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ui_signal_fn)(void);

#if IS_ENABLED(CONFIG_ZEPHCORE_UI_DESIGN_JOYSTICK)

void joystick_ui_hooks_register(void *task,
	ui_signal_fn signal_refresh,
	ui_signal_fn signal_tx);

void ui_notify_radio_stats(uint32_t pkt_recv, uint32_t pkt_sent, uint32_t pkt_errors);

void ui_signal_refresh(void);
void ui_signal_tx(void);

#else

static inline void joystick_ui_hooks_register(void *task,
	ui_signal_fn signal_refresh,
	ui_signal_fn signal_tx)
{
	ARG_UNUSED(task);
	ARG_UNUSED(signal_refresh);
	ARG_UNUSED(signal_tx);
}

static inline void ui_notify_radio_stats(uint32_t pkt_recv, uint32_t pkt_sent,
	uint32_t pkt_errors)
{
	ARG_UNUSED(pkt_recv);
	ARG_UNUSED(pkt_sent);
	ARG_UNUSED(pkt_errors);
}

static inline void ui_signal_refresh(void) {}
static inline void ui_signal_tx(void) {}

#endif /* CONFIG_ZEPHCORE_UI_DESIGN_JOYSTICK */

#ifdef __cplusplus
}
#endif
