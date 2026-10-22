/*
 * ZephCore - LVGL UI settings access
 * SPDX-License-Identifier: MIT
 *
 * Reads NodePrefs and changes them through the same CompanionMesh methods the
 * companion protocol uses, so a setting changed on the device behaves exactly
 * like the app changing it. Main thread only.
 */

#ifndef ZEPHCORE_UI_SETTINGS_H
#define ZEPHCORE_UI_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ui_settings {
	char name[32];
	int32_t lat_e6;
	int32_t lon_e6;
	uint8_t advert_loc_policy;      /* 0 = none; otherwise the stored position is sent */
	uint32_t freq_khz;
	uint32_t freq_min_khz;          /* what applyRadioParams accepts */
	uint32_t freq_max_khz;
	uint32_t bw_hz;
	uint8_t sf;
	uint8_t cr;
	int8_t tx_power;
	int8_t tx_power_max;
	uint8_t path_hash_mode;         /* 0..2 = 1..3 bytes per hop */
	bool client_repeat;             /* off-grid mode */
	bool manual_add_contacts;
	uint8_t autoadd_config;         /* AUTO_ADD_* bits */
	uint8_t autoadd_max_hops;       /* 0 = no limit, N = up to N-1 hops */
	uint8_t telemetry_base;         /* TELEM_MODE_* */
	uint8_t telemetry_loc;
	uint8_t telemetry_env;
	bool multi_acks;
	bool gps_enabled;
	uint32_t gps_interval;
	bool meshtimesync;
	uint32_t ble_pin;
	uint32_t rx_delay_x1000;
	uint32_t airtime_factor_x1000;
	int8_t tz_offset;
	uint8_t display_brightness;     /* 0 = default */
	uint16_t screen_off_secs;       /* 0 = default */
	bool wake_on_msg;
	bool display_rotate;
	uint8_t buzzer_mode;
	bool leds_disabled;
	bool ble_enabled;
};

void ui_settings_init(void *companion_mesh);
void ui_settings_read(struct ui_settings *out);

bool ui_settings_set_name(const char *name);
bool ui_settings_set_location(int32_t lat_e6, int32_t lon_e6);
bool ui_settings_set_radio(uint32_t freq_khz, uint32_t bw_hz, uint8_t sf, uint8_t cr, bool repeat);
/* Off-grid repeat is only allowed on these frequencies (OffgridPolicy.h). */
bool ui_settings_offgrid_freq_ok(uint32_t freq_khz);
bool ui_settings_set_tx_power(int8_t dbm);
void ui_settings_set_other(bool manual_add_contacts, uint8_t telemetry_base, uint8_t telemetry_loc,
			   uint8_t telemetry_env, uint8_t advert_loc_policy, bool multi_acks);
bool ui_settings_set_path_hash_mode(uint8_t mode);
void ui_settings_set_autoadd(uint8_t config, uint8_t max_hops);
bool ui_settings_set_pin(uint32_t pin);
bool ui_settings_set_custom(const char *key, const char *value);
void ui_settings_set_tuning(uint32_t rx_delay_x1000, uint32_t airtime_factor_x1000);
bool ui_settings_set_tz(int8_t hours);
void ui_settings_reboot(void);
void ui_settings_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHCORE_UI_SETTINGS_H */
