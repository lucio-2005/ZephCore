/*
 * ZephCore - LVGL UI: offline map tiles
 * SPDX-License-Identifier: MIT
 *
 * Slippy-map tiles (z/x/y, 256 x 256, Web Mercator) converted to LVGL's
 * binary image format, RGB565 (scripts/map_tiles.py). Only the native_sim
 * preview reads them so far: from $ZEPHCORE_MAP_TILES, default "tiles" in the
 * working directory. Elsewhere there are no tiles and the map draws its grid.
 * Main thread only.
 */

#ifndef ZEPHCORE_UI_MAP_TILES_H
#define ZEPHCORE_UI_MAP_TILES_H

#include <stdbool.h>
#include <stdint.h>
#include <lvgl.h>

#define MAP_TILE_SIZE 256

/* The tile, or NULL if there is none. Valid until map_tiles_keep() is next
 * called without it. */
const lv_image_dsc_t *map_tile_get(int z, int x, int y);

/* Before a frame: tiles outside [x0..x1] x [y0..y1] at zoom z may be freed. */
void map_tiles_keep(int z, int x0, int y0, int x1, int y1);

/* True if a tile folder exists. */
bool map_tiles_available(void);

/* The folder's attribution.txt (first line), or "" */
const char *map_tiles_attribution(void);

#endif /* ZEPHCORE_UI_MAP_TILES_H */
