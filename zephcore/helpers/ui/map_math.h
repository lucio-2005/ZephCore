/*
 * SPDX-License-Identifier: MIT
 *
 * Map arithmetic for the touch UI: Web Mercator as slippy-map tiles use it
 * (256-pixel tiles, zoom z is 256 * 2^z pixels around), distances, the scale
 * bar and the zoom that fits a set of points. Positions are microdegrees, as
 * adverts carry them. No Zephyr dependencies (host-tested).
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_ZOOM_MIN 1
#define MAP_ZOOM_MAX 18

/* World pixel position at zoom z; latitude is clamped to +-85.0511. */
void map_project(int32_t lat_e6, int32_t lon_e6, int z, double *x, double *y);

/* Great-circle distance in metres. */
double map_distance_m(int32_t lat1_e6, int32_t lon1_e6, int32_t lat2_e6, int32_t lon2_e6);

/* "850 m", "3.2 km", "120 km" */
void map_fmt_distance(double m, char *out, unsigned cap);

/* The longest round distance (1, 2 or 5 x 10^n m) that is at most max_px at
 * this latitude and zoom: its length in pixels and its label. */
int map_scale(int32_t lat_e6, int z, int max_px, char *label, unsigned cap);

/* The largest zoom at which a box of zoom-0 world pixels fits in w x h. */
int map_fit_zoom(double x0, double y0, double x1, double y1, int w, int h);

#ifdef __cplusplus
}
#endif
