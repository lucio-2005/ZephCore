/*
 * SPDX-License-Identifier: MIT
 * See map_math.h.
 */

#include "map_math.h"

#include <math.h>
#include <stdio.h>

#define PI 3.14159265358979323846
#define EARTH_R_M 6371008.8
#define MERCATOR_LAT_MAX 85.0511287798

void map_project(int32_t lat_e6, int32_t lon_e6, int z, double *x, double *y)
{
	double world = 256.0 * (double)(1u << z);
	double lat = lat_e6 / 1e6;
	double lon = lon_e6 / 1e6;

	if (lat > MERCATOR_LAT_MAX) {
		lat = MERCATOR_LAT_MAX;
	} else if (lat < -MERCATOR_LAT_MAX) {
		lat = -MERCATOR_LAT_MAX;
	}
	double s = sin(lat * PI / 180.0);

	*x = (lon + 180.0) / 360.0 * world;
	*y = (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * PI)) * world;
}

double map_distance_m(int32_t lat1_e6, int32_t lon1_e6, int32_t lat2_e6, int32_t lon2_e6)
{
	double p1 = lat1_e6 / 1e6 * PI / 180.0;
	double p2 = lat2_e6 / 1e6 * PI / 180.0;
	double dp = p2 - p1;
	double dl = (lon2_e6 - lon1_e6) / 1e6 * PI / 180.0;
	double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);

	return 2.0 * EARTH_R_M * atan2(sqrt(a), sqrt(1.0 - a));
}

void map_fmt_distance(double m, char *out, unsigned cap)
{
	if (m < 1000.0) {
		snprintf(out, cap, "%d m", (int)(m + 0.5));
	} else if (m < 99950.0) {
		int tenths = (int)(m / 100.0 + 0.5);

		snprintf(out, cap, "%d.%d km", tenths / 10, tenths % 10);
	} else {
		snprintf(out, cap, "%d km", (int)(m / 1000.0 + 0.5));
	}
}

int map_scale(int32_t lat_e6, int z, int max_px, char *label, unsigned cap)
{
	/* Metres per pixel along the parallel. */
	double mpp = 2.0 * PI * EARTH_R_M * cos(lat_e6 / 1e6 * PI / 180.0) / (256.0 * (double)(1u << z));
	double best = 0;

	for (double p = 1; p <= 1e7; p *= 10) {
		static const double steps[] = {1, 2, 5};

		for (int i = 0; i < 3; i++) {
			if (p * steps[i] / mpp <= max_px) {
				best = p * steps[i];
			}
		}
	}
	if (best == 0) {
		best = 1;
	}
	if (best < 1000) {
		snprintf(label, cap, "%d m", (int)best);
	} else {
		snprintf(label, cap, "%d km", (int)(best / 1000));
	}
	return (int)(best / mpp + 0.5);
}

int map_fit_zoom(double x0, double y0, double x1, double y1, int w, int h)
{
	double bw = x1 - x0, bh = y1 - y0;

	for (int z = MAP_ZOOM_MAX; z > MAP_ZOOM_MIN; z--) {
		double f = (double)(1u << z);

		if (bw * f <= w && bh * f <= h) {
			return z;
		}
	}
	return MAP_ZOOM_MIN;
}
