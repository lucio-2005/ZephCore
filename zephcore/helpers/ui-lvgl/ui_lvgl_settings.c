/*
 * ZephCore - LVGL touch UI: the Settings screens
 * SPDX-License-Identifier: MIT
 *
 * The settings the MeshCore app can change, grouped the way the app groups
 * them, plus the device-only ones (GPS, time zone, buzzer, LEDs). Values come
 * from NodePrefs through ui_settings and go back through the CompanionMesh
 * methods the app's commands use, so a change made here behaves as if the app
 * had made it.
 *
 * Most settings apply as soon as they change. Radio settings are staged and
 * applied together on Save: applying frequency, bandwidth and spreading factor
 * one at a time would take the node off its mesh after the first change. Text
 * and number settings open an edit panel on the top layer.
 *
 * Main thread only.
 */

#include <zephyr/kernel.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "ui_task.h"
#include "ui_mesh_actions.h"
#include "ui_settings.h"
#include "ui_lvgl_priv.h"
#include "mcui_gen.h"
#include <helpers/buzzer_gate.h>

#ifdef CONFIG_ZEPHCORE_UI_BUZZER
#include "buzzer.h"
#endif

/* AUTO_ADD_* in app/CompanionMesh.h (a C++ header). */
#define AA_OVERWRITE_OLDEST	BIT(0)
#define AA_CHAT			BIT(1)
#define AA_REPEATER		BIT(2)
#define AA_ROOM			BIT(3)
#define AA_SENSOR		BIT(4)

#define TZ_FIRST	(-12)	/* first entry of the time zone dropdown */
#define MAXHOPS_LAST	8	/* last entry of the max distance dropdown */
#define CONFIRM_MS	4000

/* The app's radio presets (api.meshcore.nz/api/v1/config), in its order.
 * path_hash_size 0 = the preset does not set one. */
struct radio_preset {
	const char *name;
	uint32_t freq_khz;
	uint32_t bw_hz;
	uint8_t sf;
	uint8_t cr;
	uint8_t path_hash_size;
};

static const struct radio_preset presets[] = {
	{"Australia", 915800, 250000, 10, 5, 0},
	{"Australia (Narrow)", 916575, 62500, 7, 7, 0},
	{"Australia (Mid)", 915075, 125000, 9, 5, 0},
	{"Australia: SA, WA", 923125, 62500, 8, 8, 0},
	{"Australia: QLD", 923125, 62500, 8, 5, 0},
	{"Brazil", 923125, 62500, 8, 8, 0},
	{"Canada", 910525, 62500, 7, 5, 3},
	{"Costa Rica", 910525, 125000, 11, 5, 0},
	{"EU/UK (Narrow)", 869618, 62500, 8, 8, 0},
	{"EU/UK (Deprecated)", 869525, 250000, 11, 5, 0},
	{"Czech Republic (Narrow)", 869432, 62500, 7, 5, 2},
	{"EU 433MHz (Long Range)", 433650, 250000, 11, 5, 0},
	{"EU 433MHz (Narrow)", 433650, 62500, 8, 8, 0},
	{"Hungary", 869618, 62500, 7, 5, 2},
	{"Netherlands", 869618, 62500, 7, 5, 0},
	{"Netherlands (Limburg)", 869618, 62500, 8, 8, 2},
	{"New Zealand (Narrow)", 917375, 62500, 7, 5, 2},
	{"New Zealand (Gisborne)", 917375, 250000, 11, 5, 1},
	{"Portugal 433", 433375, 62500, 9, 6, 0},
	{"Portugal 868", 869618, 62500, 7, 6, 0},
	{"Slovakia", 869618, 62500, 7, 5, 2},
	{"Switzerland", 869618, 62500, 8, 8, 0},
	{"USA", 910525, 62500, 7, 5, 0},
	{"USA - Southern California", 927875, 62500, 7, 5, 3},
	{"Vietnam (Narrow)", 920250, 62500, 8, 5, 0},
	{"Vietnam (Deprecated)", 920250, 250000, 11, 5, 0},
};

/* The bandwidth dropdown, in order. */
static const uint32_t bw_hz_tab[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};

/* Text and number settings: tapping their row opens the edit panel. */
struct edit_spec {
	const char *key;
	const char *title;
	const char *hint;
	bool number;
	uint8_t max_len;
};

static const struct edit_spec edit_specs[] = {
	{"name", "Name", "Shown to other nodes. Up to 31 characters.", false, 31},
	{"lat", "Latitude", "Decimal degrees, -90 to 90", true, 11},
	{"lon", "Longitude", "Decimal degrees, -180 to 180", true, 11},
	{"freq", "Frequency (MHz)", "Applied when you press Save", true, 8},
	{"gps_interval", "GPS interval (s)", "Seconds between fixes, 1 to 86400", true, 5},
	{"pin", "Bluetooth PIN", "6 digits, or 0 for a random PIN", true, 6},
	{"rx_delay", "RX delay base", "Seconds, 0 to 3600. 0 turns it off", true, 8},
	{"airtime", "Airtime factor", "0 to 9. Duty cycle is 100 / (factor + 1) %", true, 5},
};

static struct ui_settings cur;
static bool loading;
static bool radio_dirty;	/* radio screen has unsaved changes */
static uint32_t radio_freq_khz;	/* staged frequency */
static const char *edit_key;	/* the setting the edit panel is open for */
static int confirm_what;
static int64_t confirm_until;

/* ========== Small helpers ========== */

static void set_str(lv_subject_t *s, const char *v)
{
	if (strcmp(lv_subject_get_string(s), v) != 0) {
		lv_subject_set_string(s, v);
	}
}

static void set_int(lv_subject_t *s, int32_t v)
{
	if (lv_subject_get_int(s) != v) {
		lv_subject_set_int(s, v);
	}
}

static int32_t get_int(lv_subject_t *s)
{
	return lv_subject_get_int(s);
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* value / 10^digits as text, without floating point. */
static void fmt_fixed(char *out, size_t len, int32_t value, int digits)
{
	uint32_t scale = 1;
	uint32_t a = value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;

	for (int i = 0; i < digits; i++) {
		scale *= 10;
	}
	if (digits == 0) {
		snprintf(out, len, "%s%u", value < 0 ? "-" : "", (unsigned)a);
	} else {
		snprintf(out, len, "%s%u.%0*u", value < 0 ? "-" : "", (unsigned)(a / scale), digits,
			 (unsigned)(a % scale));
	}
}

/* Parse "-12.345" into value * 10^digits. Extra decimals are dropped. */
static bool parse_fixed(const char *s, int digits, int64_t *out)
{
	bool neg = false, any = false;
	int64_t v = 0;
	int frac = -1;

	while (*s == ' ') {
		s++;
	}
	if (*s == '-') {
		neg = true;
		s++;
	}
	for (; *s && *s != ' '; s++) {
		if (*s == '.' && frac < 0) {
			frac = 0;
		} else if (*s >= '0' && *s <= '9') {
			any = true;
			if (frac >= digits) {
				continue;
			}
			v = v * 10 + (*s - '0');
			if (frac >= 0) {
				frac++;
			}
			if (v > 100000000000LL) {
				return false;
			}
		} else {
			return false;
		}
	}
	while (*s == ' ') {
		s++;
	}
	if (!any || *s) {
		return false;
	}
	for (int i = frac < 0 ? 0 : frac; i < digits; i++) {
		v *= 10;
	}
	*out = neg ? -v : v;
	return true;
}

/* "62.5", "125", "31.25" */
static void fmt_khz(char *out, size_t len, uint32_t hz)
{
	snprintf(out, len, "%u.%03u", (unsigned)(hz / 1000), (unsigned)(hz % 1000));
	char *end = out + strlen(out) - 1;

	while (*end == '0') {
		*end-- = '\0';
	}
	if (*end == '.') {
		*end = '\0';
	}
}

static void fmt_mhz(char *out, size_t len, uint32_t khz)
{
	snprintf(out, len, "%u.%03u", (unsigned)(khz / 1000), (unsigned)(khz % 1000));
}

static void fmt_tz(char *out, size_t len, int hours)
{
	if (hours == 0) {
		snprintf(out, len, "UTC");
	} else {
		snprintf(out, len, "UTC%+d", hours);
	}
}

static int bw_index(uint32_t hz)
{
	int best = 0;
	uint32_t best_d = UINT32_MAX;

	for (int i = 0; i < (int)ARRAY_SIZE(bw_hz_tab); i++) {
		uint32_t d = hz > bw_hz_tab[i] ? hz - bw_hz_tab[i] : bw_hz_tab[i] - hz;

		if (d < best_d) {
			best_d = d;
			best = i;
		}
	}
	return best;
}

/* Dropdown index of the preset these values match (0 = Custom). Presets that
 * differ only in path hash size are told apart by the current one. */
static int preset_match(uint32_t freq_khz, uint32_t bw_hz, uint8_t sf, uint8_t cr, uint8_t path_hash_mode)
{
	int loose = 0;

	for (int i = 0; i < (int)ARRAY_SIZE(presets); i++) {
		const struct radio_preset *p = &presets[i];

		if (p->freq_khz != freq_khz || p->bw_hz != bw_hz || p->sf != sf || p->cr != cr) {
			continue;
		}
		if (p->path_hash_size == path_hash_mode + 1) {
			return i + 1;
		}
		if (!loose && p->path_hash_size == 0) {
			loose = i + 1;
		}
	}
	return loose;
}

static void staged_preset_update(void)
{
	bool was = loading;

	loading = true;
	set_int(mcui_set_preset, preset_match(radio_freq_khz, bw_hz_tab[get_int(mcui_set_bw)],
					      (uint8_t)(get_int(mcui_set_sf) + 5),
					      (uint8_t)(get_int(mcui_set_cr) + 5), cur.path_hash_mode));
	loading = was;
}

static void set_staged_freq(uint32_t khz)
{
	char buf[24];

	radio_freq_khz = khz;
	fmt_mhz(buf, sizeof(buf), khz);
	strcat(buf, " MHz");
	set_str(mcui_set_freq, buf);
}

/* ========== Summaries on the Settings menu ========== */

static const char *const tel_txt[] = {"nobody", "allowed", "everyone"};
static const char *const buzzer_txt[] = {"off", "on", "vibrate", "sound only"};

static void update_summaries(void)
{
	char buf[64], tmp[24];
	size_t n;

	snprintf(buf, sizeof(buf), "%s | %s", lv_subject_get_string(mcui_set_name),
		 get_int(mcui_set_share_loc) ? "location shared" : "location private");
	set_str(mcui_sum_public, buf);

	fmt_mhz(tmp, sizeof(tmp), cur.freq_khz);
	n = (size_t)snprintf(buf, sizeof(buf), "%s MHz | ", tmp);
	fmt_khz(tmp, sizeof(tmp), cur.bw_hz);
	snprintf(buf + n, sizeof(buf) - n, "%s kHz | SF%u | %d dBm", tmp, cur.sf, cur.tx_power);
	set_str(mcui_sum_radio, buf);

	if (get_int(mcui_set_aa_all)) {
		n = (size_t)snprintf(buf, sizeof(buf), "Add every node");
	} else {
		static const struct {
			lv_subject_t **subject;
			const char *name;
		} types[] = {
			{&mcui_set_aa_chat, "companions"}, {&mcui_set_aa_rpt, "repeaters"},
			{&mcui_set_aa_room, "rooms"}, {&mcui_set_aa_sensor, "sensors"},
		};
		bool any = false;

		n = (size_t)snprintf(buf, sizeof(buf), "Add");
		for (size_t i = 0; i < ARRAY_SIZE(types); i++) {
			if (get_int(*types[i].subject)) {
				n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s %s", any ? "," : "",
						      types[i].name);
				any = true;
			}
		}
		if (!any) {
			n = (size_t)snprintf(buf, sizeof(buf), "Add nodes by hand only");
		}
	}
	int hops = get_int(mcui_set_aa_maxhops);

	if (n < sizeof(buf) && hops == 1) {
		snprintf(buf + n, sizeof(buf) - n, " | direct only");
	} else if (n < sizeof(buf) && hops > 1) {
		snprintf(buf + n, sizeof(buf) - n, " | up to %d hops", hops - 1);
	}
	set_str(mcui_sum_contacts, buf);

	snprintf(buf, sizeof(buf), "Base %s, location %s, sensors %s",
		 tel_txt[clamp(get_int(mcui_set_tel_base), 0, 2)],
		 tel_txt[clamp(get_int(mcui_set_tel_loc), 0, 2)],
		 tel_txt[clamp(get_int(mcui_set_tel_env), 0, 2)]);
	set_str(mcui_sum_telemetry, buf);

	fmt_tz(tmp, sizeof(tmp), get_int(mcui_set_tz) + TZ_FIRST);
	snprintf(buf, sizeof(buf), "GPS %s | %s%s", get_int(mcui_gps_on) ? "on" : "off", tmp,
		 get_int(mcui_set_timesync) ? " | mesh time" : "");
	set_str(mcui_sum_location, buf);

#ifdef CONFIG_ZEPHCORE_UI_BUZZER
	snprintf(buf, sizeof(buf), "Buzzer %s | LEDs %s", buzzer_txt[clamp(get_int(mcui_set_buzzer), 0, 3)],
		 get_int(mcui_set_leds) ? "on" : "off");
#else
	ARG_UNUSED(buzzer_txt);
	snprintf(buf, sizeof(buf), "LEDs %s", get_int(mcui_set_leds) ? "on" : "off");
#endif
	set_str(mcui_sum_device, buf);

	snprintf(buf, sizeof(buf), "%s | PIN %s", get_int(mcui_ble_on) ? "On" : "Off",
		 lv_subject_get_string(mcui_set_pin));
	set_str(mcui_sum_bluetooth, buf);

	snprintf(buf, sizeof(buf), "%d-byte path hash | %s", get_int(mcui_set_path_hash) + 1,
		 get_int(mcui_set_multi_acks) ? "extra ACKs" : "single ACKs");
	set_str(mcui_sum_advanced, buf);
}

/* ========== NodePrefs -> subjects ========== */

static void load_radio(void)
{
	set_staged_freq(cur.freq_khz);
	set_int(mcui_set_bw, bw_index(cur.bw_hz));
	set_int(mcui_set_sf, clamp(cur.sf, 5, 12) - 5);
	set_int(mcui_set_cr, clamp(cur.cr, 5, 8) - 5);
	set_int(mcui_set_txp, cur.tx_power);
	set_int(mcui_set_offgrid, cur.client_repeat);
	set_int(mcui_set_preset, preset_match(cur.freq_khz, cur.bw_hz, cur.sf, cur.cr, cur.path_hash_mode));
}

void ui_lvgl_settings_load(void)
{
	char buf[32];

	ui_settings_read(&cur);
	loading = true;

	set_str(mcui_set_name, cur.name);
	fmt_fixed(buf, sizeof(buf), cur.lat_e6, 6);
	set_str(mcui_set_lat, buf);
	fmt_fixed(buf, sizeof(buf), cur.lon_e6, 6);
	set_str(mcui_set_lon, buf);
	set_int(mcui_set_share_loc, cur.advert_loc_policy != 0);

	if (!radio_dirty) {
		load_radio();
	}

	set_int(mcui_set_aa_all, !cur.manual_add_contacts);
	set_int(mcui_set_aa_chat, (cur.autoadd_config & AA_CHAT) != 0);
	set_int(mcui_set_aa_rpt, (cur.autoadd_config & AA_REPEATER) != 0);
	set_int(mcui_set_aa_room, (cur.autoadd_config & AA_ROOM) != 0);
	set_int(mcui_set_aa_sensor, (cur.autoadd_config & AA_SENSOR) != 0);
	set_int(mcui_set_aa_overwrite, (cur.autoadd_config & AA_OVERWRITE_OLDEST) != 0);
	set_int(mcui_set_aa_maxhops, clamp(cur.autoadd_max_hops, 0, MAXHOPS_LAST));

	set_int(mcui_set_tel_base, clamp(cur.telemetry_base, 0, 2));
	set_int(mcui_set_tel_loc, clamp(cur.telemetry_loc, 0, 2));
	set_int(mcui_set_tel_env, clamp(cur.telemetry_env, 0, 2));

	snprintf(buf, sizeof(buf), "every %u s", (unsigned)cur.gps_interval);
	set_str(mcui_set_gps_interval, buf);
	set_int(mcui_set_timesync, cur.meshtimesync);
	set_int(mcui_set_tz, cur.tz_offset - TZ_FIRST);

	set_int(mcui_set_buzzer, zephcore_buzzer_has_vibrate() ? clamp(cur.buzzer_mode, 0, 3)
							       : (cur.buzzer_mode != 0));
	set_int(mcui_set_leds, !cur.leds_disabled);

	if (cur.ble_pin) {
		snprintf(buf, sizeof(buf), "%06u", (unsigned)cur.ble_pin);
	} else {
		snprintf(buf, sizeof(buf), "random");
	}
	set_str(mcui_set_pin, buf);

	set_int(mcui_set_multi_acks, cur.multi_acks);
	set_int(mcui_set_path_hash, clamp(cur.path_hash_mode, 0, 2));
	fmt_fixed(buf, sizeof(buf), (int32_t)cur.rx_delay_x1000, 3);
	set_str(mcui_set_rx_delay, buf);
	fmt_fixed(buf, sizeof(buf), (int32_t)cur.airtime_factor_x1000, 3);
	set_str(mcui_set_airtime, buf);

	loading = false;
	update_summaries();
}

/* ========== Subjects -> NodePrefs ========== */

static void apply_other(void)
{
	uint8_t policy = 0;

	if (get_int(mcui_set_share_loc)) {
		policy = cur.advert_loc_policy ? cur.advert_loc_policy : 1;
	}
	ui_settings_set_other(!get_int(mcui_set_aa_all), (uint8_t)get_int(mcui_set_tel_base),
			      (uint8_t)get_int(mcui_set_tel_loc), (uint8_t)get_int(mcui_set_tel_env), policy,
			      get_int(mcui_set_multi_acks) != 0);
}

static void apply_autoadd(void)
{
	uint8_t cfg = 0;

	cfg |= get_int(mcui_set_aa_overwrite) ? AA_OVERWRITE_OLDEST : 0;
	cfg |= get_int(mcui_set_aa_chat) ? AA_CHAT : 0;
	cfg |= get_int(mcui_set_aa_rpt) ? AA_REPEATER : 0;
	cfg |= get_int(mcui_set_aa_room) ? AA_ROOM : 0;
	cfg |= get_int(mcui_set_aa_sensor) ? AA_SENSOR : 0;
	ui_settings_set_autoadd(cfg, (uint8_t)get_int(mcui_set_aa_maxhops));
}

static void apply_buzzer(uint8_t mode)
{
#ifdef CONFIG_ZEPHCORE_UI_BUZZER
	/* As the button UI: confirm while still audible, then save. */
	if (zephcore_buzzer_mode_audible(mode)) {
		zephcore_buzzer_set_mode(mode, false);
		buzzer_play(MELODY_BUZZER_ON);
	} else {
		buzzer_play(MELODY_BUZZER_OFF);
		zephcore_buzzer_set_mode(mode, true);
	}
	mesh_set_buzzer_mode(mode);
#else
	ARG_UNUSED(mode);
#endif
}

static void apply_offgrid(bool on)
{
	if (on && !mesh_offgrid_available()) {
		ui_lvgl_toast(true, "Off-grid repeat isn't allowed in this band");
		loading = true;
		lv_subject_set_int(mcui_set_offgrid, 0);
		loading = false;
		return;
	}
	/* Same path as the device toggle: retunes to the off-grid frequency
	 * and back, then ui_set_offgrid_mode() reloads the screen. */
	radio_dirty = false;
	mesh_set_offgrid_mode(on);
	ui_lvgl_toast(false, on ? "Off-grid repeat on" : "Off-grid repeat off");
}

static void preset_selected(int index)
{
	if (index <= 0 || index > (int)ARRAY_SIZE(presets)) {
		return;
	}
	const struct radio_preset *p = &presets[index - 1];

	loading = true;
	set_staged_freq(p->freq_khz);
	set_int(mcui_set_bw, bw_index(p->bw_hz));
	set_int(mcui_set_sf, p->sf - 5);
	set_int(mcui_set_cr, p->cr - 5);
	loading = false;
	radio_dirty = true;
}

static void on_change(lv_observer_t *observer, lv_subject_t *s)
{
	int32_t v = lv_subject_get_int(s);

	ARG_UNUSED(observer);
	if (loading) {
		return;
	}
	if (s == mcui_set_share_loc || s == mcui_set_aa_all || s == mcui_set_tel_base ||
	    s == mcui_set_tel_loc || s == mcui_set_tel_env || s == mcui_set_multi_acks) {
		apply_other();
	} else if (s == mcui_set_aa_chat || s == mcui_set_aa_rpt || s == mcui_set_aa_room ||
		   s == mcui_set_aa_sensor || s == mcui_set_aa_overwrite || s == mcui_set_aa_maxhops) {
		apply_autoadd();
	} else if (s == mcui_set_timesync) {
		ui_settings_set_custom("meshtimesync", v ? "1" : "0");
	} else if (s == mcui_set_tz) {
		if (ui_settings_set_tz((int8_t)(v + TZ_FIRST))) {
			ui_set_tz((int8_t)(v + TZ_FIRST));
		}
	} else if (s == mcui_set_buzzer) {
		apply_buzzer((uint8_t)v);
	} else if (s == mcui_set_leds) {
		ui_set_leds_disabled(!v);
		mesh_set_leds_disabled(!v);
	} else if (s == mcui_set_path_hash) {
		ui_settings_set_path_hash_mode((uint8_t)v);
	} else if (s == mcui_set_preset) {
		preset_selected(v);
	} else if (s == mcui_set_bw || s == mcui_set_sf || s == mcui_set_cr) {
		radio_dirty = true;
		staged_preset_update();
	} else if (s == mcui_set_txp) {
		radio_dirty = true;
	} else if (s == mcui_set_offgrid) {
		apply_offgrid(v != 0);
	}
	/* Applied synchronously: re-read so the summaries show what stuck. */
	ui_settings_read(&cur);
	update_summaries();
}

/* gps_on and ble_on are applied by ui_lvgl.c; only the summaries follow them. */
static void on_summary_input(lv_observer_t *observer, lv_subject_t *s)
{
	ARG_UNUSED(observer);
	ARG_UNUSED(s);
	if (!loading) {
		update_summaries();
	}
}

void ui_on_radio_save(lv_event_t *e)
{
	char msg[64];
	uint32_t bw = bw_hz_tab[clamp(get_int(mcui_set_bw), 0, (int32_t)ARRAY_SIZE(bw_hz_tab) - 1)];
	uint8_t sf = (uint8_t)(get_int(mcui_set_sf) + 5);
	uint8_t cr = (uint8_t)(get_int(mcui_set_cr) + 5);
	int8_t txp = (int8_t)get_int(mcui_set_txp);
	int preset = get_int(mcui_set_preset);

	ARG_UNUSED(e);
	if (radio_freq_khz < cur.freq_min_khz || radio_freq_khz > cur.freq_max_khz) {
		snprintf(msg, sizeof(msg), "Not saved: frequency must be %u to %u MHz",
			 (unsigned)(cur.freq_min_khz / 1000), (unsigned)(cur.freq_max_khz / 1000));
		ui_lvgl_toast(true, msg);
		return;
	}
	if (cur.client_repeat && !ui_settings_offgrid_freq_ok(radio_freq_khz)) {
		ui_lvgl_toast(true, "Not saved: turn off off-grid repeat first");
		return;
	}
	if (!ui_settings_set_radio(radio_freq_khz, bw, sf, cr, cur.client_repeat)) {
		ui_lvgl_toast(true, "Not saved: the radio rejected these values");
		return;
	}
	if (txp != cur.tx_power && !ui_settings_set_tx_power(txp)) {
		ui_lvgl_toast(true, "Radio saved, but not the TX power");
	} else {
		ui_lvgl_toast(false, "Radio settings saved");
	}
	if (preset > 0 && preset <= (int)ARRAY_SIZE(presets) && presets[preset - 1].path_hash_size) {
		ui_settings_set_path_hash_mode(presets[preset - 1].path_hash_size - 1);
	}
	radio_dirty = false;
	ui_lvgl_settings_load();
}

/* ========== Screens ========== */

void ui_on_settings_open(lv_event_t *e)
{
	const char *which = lv_event_get_user_data(e);
	lv_obj_t *scr = lv_event_get_current_target(e);
	lv_obj_t *dd = NULL;

	if (!which || strcmp(which, "menu") == 0 || strcmp(which, "radio") == 0) {
		/* Leaving the radio screen without Save drops its changes. */
		radio_dirty = false;
	}
	if (which && strcmp(which, "menu") == 0) {
		/* Back on the home screen: the Contacts tab may be stale too. */
		ui_lvgl_contacts_refresh(false);
	}
	if (which && strcmp(which, "radio") == 0) {
		lv_obj_t *slider = lv_obj_find_by_name(scr, "radio_txp");

		ui_settings_read(&cur);
		dd = lv_obj_find_by_name(scr, "radio_preset");
		if (dd) {
			lv_dropdown_set_options(dd, "Custom");
			for (size_t i = 0; i < ARRAY_SIZE(presets); i++) {
				lv_dropdown_add_option(dd, presets[i].name, LV_DROPDOWN_POS_LAST);
			}
		}
		if (slider) {
			lv_subject_set_max_value_int(mcui_set_txp, cur.tx_power_max);
			lv_slider_set_range(slider, -9, cur.tx_power_max);
		}
	} else if (which && strcmp(which, "device") == 0) {
		lv_obj_t *row = lv_obj_find_by_name(scr, "row_buzzer");

		dd = lv_obj_find_by_name(scr, "dev_buzzer");
		if (dd && !zephcore_buzzer_has_vibrate()) {
			lv_dropdown_set_options(dd, "Off\nOn");
		}
		if (row && !IS_ENABLED(CONFIG_ZEPHCORE_UI_BUZZER)) {
			lv_obj_set_hidden(row, true);
		}
		confirm_what = 0;
	}

	ui_lvgl_settings_load();

	/* Replacing a dropdown's options resets its selection. */
	if (dd) {
		lv_dropdown_set_selected(dd, (uint32_t)get_int(which[0] == 'r' ? mcui_set_preset : mcui_set_buzzer));
	}
}

static void confirm_then(int what, const char *again, void (*action)(void))
{
	int64_t now = k_uptime_get();

	if (confirm_what == what && now < confirm_until) {
		mcui_notice_overlay_create(lv_layer_top(), what == 2 ? "Factory reset" : "Restarting",
					   what == 2 ? "Erasing and restarting" : "Back in a few seconds");
		lv_refr_now(NULL);
		action();
		return;
	}
	confirm_what = what;
	confirm_until = now + CONFIRM_MS;
	ui_lvgl_toast(what == 2, again);
}

void ui_on_reboot(lv_event_t *e)
{
	ARG_UNUSED(e);
	confirm_then(1, "Tap Reboot again to restart", ui_settings_reboot);
}

void ui_on_factory_reset(lv_event_t *e)
{
	ARG_UNUSED(e);
	confirm_then(2, "Tap Factory reset again to erase everything", ui_settings_factory_reset);
}

/* ========== Text and number settings ========== */

static bool settings_save(const char *txt, const char **err)
{
	const char *key = edit_key;

	char buf[16];
	int64_t v;

	if (strcmp(key, "name") == 0) {
		*err = "Name can't be empty";
		if (!txt[0] || !ui_settings_set_name(txt)) {
			return false;
		}
		ui_set_node_name(txt);
		return true;
	}
	if (strcmp(key, "lat") == 0 || strcmp(key, "lon") == 0) {
		bool lat = key[1] == 'a';
		int64_t lim = lat ? 90000000 : 180000000;

		*err = lat ? "Latitude must be -90 to 90" : "Longitude must be -180 to 180";
		if (!parse_fixed(txt, 6, &v) || v < -lim || v > lim) {
			return false;
		}
		return ui_settings_set_location(lat ? (int32_t)v : cur.lat_e6, lat ? cur.lon_e6 : (int32_t)v);
	}
	if (strcmp(key, "freq") == 0) {
		static char range[48];

		snprintf(range, sizeof(range), "Frequency must be %u to %u MHz",
			 (unsigned)(cur.freq_min_khz / 1000), (unsigned)(cur.freq_max_khz / 1000));
		*err = range;
		if (!parse_fixed(txt, 3, &v) || v < cur.freq_min_khz || v > cur.freq_max_khz) {
			return false;
		}
		/* Staged; Save applies it with the rest of the radio settings. */
		set_staged_freq((uint32_t)v);
		radio_dirty = true;
		staged_preset_update();
		return true;
	}
	if (strcmp(key, "gps_interval") == 0) {
		*err = "GPS interval must be 1 to 86400 seconds";
		if (!parse_fixed(txt, 0, &v) || v < 1 || v > 86400) {
			return false;
		}
		snprintf(buf, sizeof(buf), "%u", (unsigned)v);
		return ui_settings_set_custom("gps_interval", buf);
	}
	if (strcmp(key, "pin") == 0) {
		*err = "PIN must be 6 digits, or 0";
		return parse_fixed(txt, 0, &v) && v >= 0 && v <= 999999 && ui_settings_set_pin((uint32_t)v);
	}
	if (strcmp(key, "rx_delay") == 0) {
		/* The range loadPrefs() keeps (NodePrefs.h). */
		*err = "RX delay must be 0 to 3600";
		if (!parse_fixed(txt, 3, &v) || v < 0 || v > 3600000) {
			return false;
		}
		ui_settings_set_tuning((uint32_t)v, cur.airtime_factor_x1000);
		return true;
	}
	if (strcmp(key, "airtime") == 0) {
		*err = "Airtime factor must be 0 to 9";
		if (!parse_fixed(txt, 3, &v) || v < 0 || v > 9000) {
			return false;
		}
		ui_settings_set_tuning(cur.rx_delay_x1000, (uint32_t)v);
		return true;
	}
	*err = "Unknown setting";
	return false;
}

static bool settings_save_and_reload(const char *txt, const char **err)
{
	if (!settings_save(txt, err)) {
		return false;
	}
	ui_lvgl_settings_load();
	return true;
}

void ui_lvgl_settings_edit(const char *key)
{
	const struct edit_spec *spec = NULL;
	char buf[40];

	for (size_t i = 0; i < ARRAY_SIZE(edit_specs); i++) {
		if (strcmp(edit_specs[i].key, key) == 0) {
			spec = &edit_specs[i];
		}
	}
	if (!spec) {
		return;
	}

	/* The current value, as a plain number for number fields. */
	buf[0] = '\0';
	if (strcmp(key, "name") == 0) {
		snprintf(buf, sizeof(buf), "%s", cur.name);
	} else if (strcmp(key, "lat") == 0) {
		snprintf(buf, sizeof(buf), "%s", lv_subject_get_string(mcui_set_lat));
	} else if (strcmp(key, "lon") == 0) {
		snprintf(buf, sizeof(buf), "%s", lv_subject_get_string(mcui_set_lon));
	} else if (strcmp(key, "freq") == 0) {
		fmt_mhz(buf, sizeof(buf), radio_freq_khz);
	} else if (strcmp(key, "gps_interval") == 0) {
		snprintf(buf, sizeof(buf), "%u", (unsigned)cur.gps_interval);
	} else if (strcmp(key, "pin") == 0 && cur.ble_pin) {
		snprintf(buf, sizeof(buf), "%06u", (unsigned)cur.ble_pin);
	} else if (strcmp(key, "rx_delay") == 0) {
		snprintf(buf, sizeof(buf), "%s", lv_subject_get_string(mcui_set_rx_delay));
	} else if (strcmp(key, "airtime") == 0) {
		snprintf(buf, sizeof(buf), "%s", lv_subject_get_string(mcui_set_airtime));
	}

	edit_key = spec->key;
	ui_lvgl_edit_open(&(struct ui_edit_req){
		.title = spec->title,
		.hint = spec->hint,
		.initial = buf,
		.number = spec->number,
		.max_len = spec->max_len,
		.save = settings_save_and_reload,
	});
}

/* ========== Init ========== */

void ui_lvgl_settings_init(void)
{
	lv_subject_t *applied[] = {
		mcui_set_share_loc, mcui_set_preset, mcui_set_bw, mcui_set_sf, mcui_set_cr, mcui_set_txp,
		mcui_set_offgrid, mcui_set_aa_all, mcui_set_aa_chat, mcui_set_aa_rpt, mcui_set_aa_room,
		mcui_set_aa_sensor, mcui_set_aa_overwrite, mcui_set_aa_maxhops, mcui_set_tel_base,
		mcui_set_tel_loc, mcui_set_tel_env, mcui_set_timesync, mcui_set_tz, mcui_set_buzzer,
		mcui_set_leds, mcui_set_multi_acks, mcui_set_path_hash,
	};

	/* Adding an observer calls it once; that call must not apply anything. */
	loading = true;
	for (size_t i = 0; i < ARRAY_SIZE(applied); i++) {
		lv_subject_add_observer(applied[i], on_change, NULL);
	}
	lv_subject_add_observer(mcui_gps_on, on_summary_input, NULL);
	lv_subject_add_observer(mcui_ble_on, on_summary_input, NULL);
	loading = false;

	if (!IS_ENABLED(CONFIG_BT)) {
		lv_obj_t *row = lv_obj_find_by_name(mcui_home, "nav_bluetooth");

		if (row) {
			lv_obj_set_hidden(row, true);
		}
	}
}
