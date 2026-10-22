/*
 * SPDX-License-Identifier: MIT
 * See telemetry_text.h.
 */

#include "telemetry_text.h"

#include <helpers/SensorManager.h>        /* TELEM_CHANNEL_SELF */
#include <helpers/sensors/LPPDataHelpers.h>

#include <stdio.h>

namespace {

/* A field's value(s), with the scales and signs of the encoder
 * (helpers/compat/CayenneLPP.h); LPPData's own tables miss a few (humidity
 * scale, current sign). False for a type that is not shown. */
bool field_value(uint8_t type, const uint8_t *v, float out[2])
{
	switch (type) {
	case LPP_VOLTAGE:             out[0] = LPPData::getFloat(v, 2, 100, true); return true;
	case LPP_TEMPERATURE:         out[0] = LPPData::getFloat(v, 2, 10, true); return true;
	case LPP_RELATIVE_HUMIDITY:   out[0] = LPPData::getFloat(v, 1, 2, false); return true;
	case LPP_BAROMETRIC_PRESSURE: out[0] = LPPData::getFloat(v, 2, 10, false); return true;
	case LPP_ALTITUDE:            out[0] = LPPData::getFloat(v, 2, 1, true); return true;
	case LPP_LUMINOSITY:          out[0] = LPPData::getFloat(v, 2, 1, false); return true;
	/* signed: a bidirectional monitor reports discharge < 0 */
	case LPP_CURRENT:             out[0] = LPPData::getFloat(v, 2, 1000, true) * 1000.0f; return true;
	case LPP_POWER:               out[0] = LPPData::getFloat(v, 2, 1, false); return true;
	case LPP_GPS:
		out[0] = LPPData::getFloat(v, 3, 10000, true);
		out[1] = LPPData::getFloat(v + 3, 3, 10000, true);
		return true;
	default:
		return false;
	}
}

/* Calls fn(channel, type, values) for each shown field until it returns
 * false. Stops at the end-of-data marker (channel 0) or a truncated field. */
template <typename F>
void walk(const uint8_t *data, uint8_t len, F fn)
{
	uint8_t pos = 0;

	while (pos + 2 < len) {
		uint8_t ch = data[pos];
		uint8_t type = data[pos + 1];
		const uint8_t *v = &data[pos + 2];
		uint8_t size = LPPData::getDataSize(type);
		float val[2];

		pos += 2;
		if (ch == 0 || pos + size > len) {
			break;
		}
		pos += size;
		if (field_value(type, v, val) && !fn(ch, type, val)) {
			break;
		}
	}
}

} /* namespace */

void telemetry_text(const uint8_t *data, uint8_t len, char *out, size_t cap)
{
	char *dp = out;
	char *end = out + cap;

	*dp = '\0';
	walk(data, len, [&](uint8_t ch, uint8_t type, const float *val) {
		if (end - dp <= 24) {
			return false;
		}
		bool self = ch == TELEM_CHANNEL_SELF;
		char pre[6] = "";
		if (!self) {
			snprintf(pre, sizeof(pre), "c%u ", ch);
		}
		const char *sep = (dp > out) ? "\n" : "";
		int n = 0;

		switch (type) {
		case LPP_VOLTAGE:
			n = snprintf(dp, end - dp, "%s%s%s%.2fV", sep, pre, self ? "batt " : "", (double)val[0]);
			break;
		case LPP_TEMPERATURE:
			n = snprintf(dp, end - dp, "%s%stemp %.1fC", sep, pre, (double)val[0]);
			break;
		case LPP_RELATIVE_HUMIDITY:
			n = snprintf(dp, end - dp, "%s%shum %.1f%%", sep, pre, (double)val[0]);
			break;
		case LPP_BAROMETRIC_PRESSURE:
			n = snprintf(dp, end - dp, "%s%spres %.1fhPa", sep, pre, (double)val[0]);
			break;
		case LPP_ALTITUDE:
			n = snprintf(dp, end - dp, "%s%salt %.0fm", sep, pre, (double)val[0]);
			break;
		case LPP_LUMINOSITY:
			n = snprintf(dp, end - dp, "%s%slight %.0f", sep, pre, (double)val[0]);
			break;
		case LPP_CURRENT:
			n = snprintf(dp, end - dp, "%s%scur %.0fmA", sep, pre, (double)val[0]);
			break;
		case LPP_POWER:
			n = snprintf(dp, end - dp, "%s%spwr %.0fW", sep, pre, (double)val[0]);
			break;
		case LPP_GPS:
			n = snprintf(dp, end - dp, "%s%sgps %.4f,%.4f", sep, pre, (double)val[0], (double)val[1]);
			break;
		}
		if (n < 0 || n >= end - dp) {
			return false;
		}
		dp += n;
		return true;
	});
	*dp = '\0';
	if (dp == out) {
		snprintf(out, cap, "no telemetry");
	}
}

int telemetry_fields(const uint8_t *data, uint8_t len, struct telemetry_field *out, int max)
{
	int count = 0;

	walk(data, len, [&](uint8_t ch, uint8_t type, const float *val) {
		if (count >= max) {
			return false;
		}
		struct telemetry_field *f = &out[count++];
		const char *label = "";

		f->channel = ch;
		switch (type) {
		case LPP_VOLTAGE:
			label = ch == TELEM_CHANNEL_SELF ? "Battery" : "Voltage";
			snprintf(f->value, sizeof(f->value), "%.2f V", (double)val[0]);
			break;
		case LPP_TEMPERATURE:
			label = "Temperature";
			snprintf(f->value, sizeof(f->value), "%.1f C", (double)val[0]);
			break;
		case LPP_RELATIVE_HUMIDITY:
			label = "Humidity";
			snprintf(f->value, sizeof(f->value), "%.1f %%", (double)val[0]);
			break;
		case LPP_BAROMETRIC_PRESSURE:
			label = "Pressure";
			snprintf(f->value, sizeof(f->value), "%.1f hPa", (double)val[0]);
			break;
		case LPP_ALTITUDE:
			label = "Altitude";
			snprintf(f->value, sizeof(f->value), "%.0f m", (double)val[0]);
			break;
		case LPP_LUMINOSITY:
			label = "Light";
			snprintf(f->value, sizeof(f->value), "%.0f lux", (double)val[0]);
			break;
		case LPP_CURRENT:
			label = "Current";
			snprintf(f->value, sizeof(f->value), "%.0f mA", (double)val[0]);
			break;
		case LPP_POWER:
			label = "Power";
			snprintf(f->value, sizeof(f->value), "%.0f W", (double)val[0]);
			break;
		case LPP_GPS:
			label = "Position";
			snprintf(f->value, sizeof(f->value), "%.5f, %.5f", (double)val[0], (double)val[1]);
			break;
		}
		snprintf(f->label, sizeof(f->label), "%s", label);
		return true;
	});
	return count;
}
