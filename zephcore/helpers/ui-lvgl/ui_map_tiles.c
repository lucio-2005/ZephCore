/*
 * ZephCore - LVGL UI: offline map tiles
 * SPDX-License-Identifier: MIT
 *
 * Tiles are read whole into memory outside the LVGL pool and drawn from there
 * (RGB565 needs no decoding). LVGL's image caches are off in this UI
 * (LV_CACHE_DEF_SIZE = 0), so a tile is only referenced by the draw tasks of
 * the frame that uses it; map_tiles_keep() frees only tiles outside the
 * current view.
 */

#include "ui_map_tiles.h"

#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>

#define CACHE_MAX	12
#define TILE_BYTES_MAX	(12 + MAP_TILE_SIZE * MAP_TILE_SIZE * 2)

struct tile {
	int z, x, y;
	uint8_t state;		/* 0 free, 1 loaded, 2 missing */
	uint32_t used;
	lv_image_dsc_t dsc;
	void *mem;
};

static struct tile cache[CACHE_MAX];
static uint32_t use_tick;

#ifdef CONFIG_ARCH_POSIX
#include <nsi_host_trampolines.h>

#define HOST_O_RDONLY 0	/* nsi_host_open() takes the host's flags */

static char root[160];
static int8_t have_root = -1;
static char attribution[64];

static const char *tiles_root(void)
{
	if (have_root < 0) {
		const char *env = nsi_host_getenv("ZEPHCORE_MAP_TILES");
		char path[200];
		int fd;

		snprintf(root, sizeof(root), "%s", env && env[0] ? env : "tiles");
		snprintf(path, sizeof(path), "%s/attribution.txt", root);
		fd = nsi_host_open(path, HOST_O_RDONLY);
		if (fd >= 0) {
			long n = nsi_host_read(fd, attribution, sizeof(attribution) - 1);

			attribution[n > 0 ? n : 0] = '\0';
			attribution[strcspn(attribution, "\r\n")] = '\0';
			nsi_host_close(fd);
		}
		/* The folder exists if any zoom level does. */
		have_root = 0;
		for (int z = 0; z <= 19 && !have_root; z++) {
			snprintf(path, sizeof(path), "%s/%d", root, z);
			fd = nsi_host_open(path, HOST_O_RDONLY);
			if (fd >= 0) {
				have_root = 1;
				nsi_host_close(fd);
			}
		}
	}
	return have_root ? root : NULL;
}

/* The whole file, or NULL. */
static void *read_tile(int z, int x, int y, long *len)
{
	char path[200];
	const char *dir = tiles_root();
	int fd;

	if (!dir) {
		return NULL;
	}
	snprintf(path, sizeof(path), "%s/%d/%d/%d.bin", dir, z, x, y);
	fd = nsi_host_open(path, HOST_O_RDONLY);
	if (fd < 0) {
		return NULL;
	}
	uint8_t *buf = nsi_host_malloc(TILE_BYTES_MAX);
	long n = 0, r;

	while (buf && n < TILE_BYTES_MAX && (r = nsi_host_read(fd, buf + n, TILE_BYTES_MAX - n)) > 0) {
		n += r;
	}
	nsi_host_close(fd);
	*len = n;
	return buf;
}

static void free_tile(void *mem)
{
	nsi_host_free(mem);
}

bool map_tiles_available(void)
{
	return tiles_root() != NULL;
}

const char *map_tiles_attribution(void)
{
	tiles_root();
	return attribution;
}

#else /* no tile storage on this platform yet */

static void *read_tile(int z, int x, int y, long *len)
{
	ARG_UNUSED(z);
	ARG_UNUSED(x);
	ARG_UNUSED(y);
	ARG_UNUSED(len);
	return NULL;
}

static void free_tile(void *mem)
{
	ARG_UNUSED(mem);
}

bool map_tiles_available(void)
{
	return false;
}

const char *map_tiles_attribution(void)
{
	return "";
}

#endif

/* An LVGL v9 binary image: 12-byte header, then RGB565 rows. */
static bool tile_valid(const uint8_t *b, long len, lv_image_dsc_t *dsc)
{
	lv_image_header_t h;

	if (len < 12) {
		return false;
	}
	memcpy(&h, b, sizeof(h));
	if (h.magic != LV_IMAGE_HEADER_MAGIC || h.cf != LV_COLOR_FORMAT_RGB565 ||
	    h.w != MAP_TILE_SIZE || h.h != MAP_TILE_SIZE || h.stride < h.w * 2 ||
	    len < 12 + (long)h.stride * h.h) {
		return false;
	}
	memset(dsc, 0, sizeof(*dsc));
	dsc->header = h;
	dsc->data = b + 12;
	dsc->data_size = (uint32_t)h.stride * h.h;
	return true;
}

const lv_image_dsc_t *map_tile_get(int z, int x, int y)
{
	struct tile *slot = NULL;

	for (int i = 0; i < CACHE_MAX; i++) {
		struct tile *t = &cache[i];

		if (t->state && t->z == z && t->x == x && t->y == y) {
			t->used = ++use_tick;
			return t->state == 1 ? &t->dsc : NULL;
		}
		if (!t->state && !slot) {
			slot = t;
		}
	}
	if (!slot) {
		/* Full of tiles in view: draw this one as grid. */
		return NULL;
	}
	long len = 0;
	void *mem = read_tile(z, x, y, &len);

	slot->z = z;
	slot->x = x;
	slot->y = y;
	slot->used = ++use_tick;
	slot->mem = NULL;
	if (mem && tile_valid(mem, len, &slot->dsc)) {
		slot->mem = mem;
		slot->state = 1;
		return &slot->dsc;
	}
	if (mem) {
		free_tile(mem);
	}
	slot->state = 2;
	return NULL;
}

void map_tiles_keep(int z, int x0, int y0, int x1, int y1)
{
	for (int i = 0; i < CACHE_MAX; i++) {
		struct tile *t = &cache[i];

		if (!t->state || (t->z == z && t->x >= x0 && t->x <= x1 && t->y >= y0 && t->y <= y1)) {
			continue;
		}
		if (t->mem) {
			free_tile(t->mem);
			t->mem = NULL;
		}
		t->state = 0;
	}
}
