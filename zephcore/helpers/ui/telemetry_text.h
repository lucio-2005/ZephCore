/*
 * SPDX-License-Identifier: MIT
 *
 * CayenneLPP telemetry for display. Channel 1 is the node itself (battery,
 * MCU temperature, GPS); every other channel is a sensor. Types not shown are
 * skipped by their size (upstream's LPPData), never a reason to stop. No
 * Zephyr dependencies (host-tested).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One field per line, sensors with a "cN " prefix (the joystick UI's admin
 * screen). Writes a NUL-terminated string; "no telemetry" when nothing was
 * shown. */
void telemetry_text(const uint8_t *data, uint8_t len, char *out, size_t cap);

struct telemetry_field {
	uint8_t channel;
	char label[16];         /* "Battery", "Temperature", ... */
	char value[28];         /* "4.12 V", "21.5 C", "47.12340, -19.50000" */
};

/* Fills up to max fields, in the order sent; returns how many. */
int telemetry_fields(const uint8_t *data, uint8_t len, struct telemetry_field *out, int max);

#ifdef __cplusplus
}
#endif
