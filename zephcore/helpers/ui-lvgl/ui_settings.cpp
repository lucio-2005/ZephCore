/*
 * ZephCore - LVGL UI settings access
 * SPDX-License-Identifier: MIT
 */

#include "ui_settings.h"

#include <CompanionMesh.h>
#include <CompanionProtocol.h>
#include <helpers/buzzer_gate.h>
#include <helpers/led_gate.h>
#include <helpers/OffgridPolicy.h>
#include <zephyr/sys/reboot.h>
#include <string.h>

static CompanionMesh *s_mesh;

extern "C" void ui_settings_init(void *companion_mesh)
{
	s_mesh = static_cast<CompanionMesh *>(companion_mesh);
}

extern "C" void ui_settings_read(struct ui_settings *out)
{
	memset(out, 0, sizeof(*out));
	if (!s_mesh) {
		return;
	}
	const NodePrefs &p = s_mesh->prefs;

	strncpy(out->name, p.node_name, sizeof(out->name) - 1);
	out->lat_e6 = (int32_t)(p.node_lat * 1000000.0);
	out->lon_e6 = (int32_t)(p.node_lon * 1000000.0);
	out->advert_loc_policy = p.advert_loc_policy;
	out->freq_khz = (uint32_t)(p.freq * 1000.0f + 0.5f);
	out->freq_min_khz = (uint32_t)(ZC_RADIO_FREQ_MIN_MHZ * 1000.0f);
	out->freq_max_khz = (uint32_t)(ZC_RADIO_FREQ_MAX_MHZ * 1000.0f);
	out->bw_hz = (uint32_t)(p.bw * 1000.0f + 0.5f);
	out->sf = p.sf;
	out->cr = p.cr;
	out->tx_power = p.tx_power_dbm;
	out->tx_power_max = MAX_LORA_TX_POWER;
	out->path_hash_mode = p.path_hash_mode;
	out->client_repeat = p.client_repeat != 0;
	out->manual_add_contacts = p.manual_add_contacts != 0;
	out->autoadd_config = p.autoadd_config;
	out->autoadd_max_hops = p.autoadd_max_hops;
	out->telemetry_base = p.telemetry_mode_base;
	out->telemetry_loc = p.telemetry_mode_loc;
	out->telemetry_env = p.telemetry_mode_env;
	out->multi_acks = p.multi_acks != 0;
	out->gps_enabled = p.gps_enabled != 0;
	out->gps_interval = p.gps_interval;
	out->meshtimesync = p.meshtimesync != 0;
	out->ble_pin = p.ble_pin;
	out->rx_delay_x1000 = (uint32_t)(p.rx_delay_base * 1000.0f + 0.5f);
	out->airtime_factor_x1000 = (uint32_t)(p.airtime_factor * 1000.0f + 0.5f);
	out->tz_offset = p.tz_offset;
	out->display_brightness = p.display_brightness;
	out->screen_off_secs = p.screen_off_secs;
	out->wake_on_msg = p.wake_on_msg != 0;
	out->display_rotate = p.display_rotate != 0;
	out->buzzer_mode = zephcore_buzzer_mode();
	out->leds_disabled = zephcore_leds_disabled();
	out->ble_enabled = p.ble_disabled == 0;
}

extern "C" bool ui_settings_set_name(const char *name)
{
	return s_mesh && s_mesh->applyAdvertName(name, strlen(name));
}

extern "C" bool ui_settings_set_location(int32_t lat_e6, int32_t lon_e6)
{
	return s_mesh && s_mesh->applyAdvertLatLon(lat_e6, lon_e6);
}

extern "C" bool ui_settings_set_radio(uint32_t freq_khz, uint32_t bw_hz, uint8_t sf, uint8_t cr, bool repeat)
{
	return s_mesh && s_mesh->applyRadioParams(freq_khz, bw_hz, sf, cr, repeat ? 1 : 0);
}

extern "C" bool ui_settings_offgrid_freq_ok(uint32_t freq_khz)
{
	return mesh::isOffgridFreqKhz(freq_khz);
}

extern "C" bool ui_settings_set_tx_power(int8_t dbm)
{
	return s_mesh && s_mesh->applyTxPower(dbm);
}

extern "C" void ui_settings_set_other(bool manual_add_contacts, uint8_t telemetry_base, uint8_t telemetry_loc,
				      uint8_t telemetry_env, uint8_t advert_loc_policy, bool multi_acks)
{
	if (!s_mesh) {
		return;
	}
	uint8_t modes = (uint8_t)(((telemetry_env & 3) << 4) | ((telemetry_loc & 3) << 2) | (telemetry_base & 3));

	s_mesh->applyOtherParams(manual_add_contacts ? 1 : 0, modes, advert_loc_policy, multi_acks ? 1 : 0);
}

extern "C" bool ui_settings_set_path_hash_mode(uint8_t mode)
{
	return s_mesh && s_mesh->applyPathHashMode(mode);
}

extern "C" void ui_settings_set_autoadd(uint8_t config, uint8_t max_hops)
{
	if (s_mesh) {
		s_mesh->applyAutoaddConfig(config, max_hops);
	}
}

extern "C" bool ui_settings_set_pin(uint32_t pin)
{
	return s_mesh && s_mesh->applyDevicePin(pin);
}

extern "C" bool ui_settings_set_custom(const char *key, const char *value)
{
	return s_mesh && s_mesh->applyCustomVar(key, value);
}

extern "C" void ui_settings_set_tuning(uint32_t rx_delay_x1000, uint32_t airtime_factor_x1000)
{
	if (s_mesh) {
		s_mesh->applyTuningParams(rx_delay_x1000, airtime_factor_x1000);
	}
}

extern "C" bool ui_settings_set_tz(int8_t hours)
{
	return s_mesh && s_mesh->applyTzOffset(hours);
}

extern "C" void ui_settings_reboot(void)
{
	if (s_mesh) {
		s_mesh->rebootDevice();
	}
	sys_reboot(SYS_REBOOT_COLD);
}

extern "C" void ui_settings_factory_reset(void)
{
	if (s_mesh) {
		s_mesh->factoryResetDevice();
	}
	sys_reboot(SYS_REBOOT_COLD);
}
