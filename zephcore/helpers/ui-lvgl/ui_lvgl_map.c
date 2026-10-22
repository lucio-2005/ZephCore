/*
 * ZephCore - LVGL touch UI: the map
 * SPDX-License-Identifier: MIT
 *
 * This node and the contacts whose adverts carry a position, newest first
 * (MAP_NODES_MAX of them, the Contacts tab's filter applied), on a Web
 * Mercator map. Drag to pan, - / + to zoom, All to fit everything. Tapping a
 * node shows its distance and when it was heard, and opens it.
 *
 * Everything is drawn by one callback on the map area, so markers cost no
 * LVGL objects: offline tiles under it when there are any (ui_map_tiles),
 * otherwise range rings around this node. Main thread only.
 */

#include <zephyr/kernel.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_remote.h"
#include "ui_settings.h"
#include "ui_lvgl_priv.h"
#include "ui_map_tiles.h"
#include "map_math.h"
#include "mcui_gen.h"

#define MAP_NODES_MAX	64
#define MARKER_R	5
#define HIT_PX		20
#define DRAG_PX		6
#define LABELS_MAX	20	/* name every marker up to this many in view */
#define KEY_PAN_PX	48
#define SEL_NONE	(-2)
#define SEL_SELF	(-1)

struct map_node {
	uint8_t key[UI_PUB_KEY_SIZE];
	char name[32];
	uint8_t type;
	int32_t lat, lon;
	uint32_t age_s;
};

static struct map_node nodes[MAP_NODES_MAX];
static int node_count;
static int with_position;	/* contacts with a position, shown or not */
static bool self_known;
static int32_t self_lat, self_lon;
static char self_name[32];

static lv_obj_t *map_scr;
static lv_obj_t *map_area;
static int zoom = 12;
static double cx, cy;		/* view centre, world pixels at this zoom */
static int selected = SEL_NONE;
static int32_t drag_px;

static lv_color_t type_color(uint8_t type)
{
	switch (type) {
	case UI_NODE_REPEATER: return MCUI_KIND_RPT;
	case UI_NODE_ROOM: return MCUI_KIND_ROOM;
	case UI_NODE_SENSOR: return MCUI_KIND_SENSOR;
	default: return MCUI_KIND_CHAT;
	}
}

static void load_nodes(void)
{
	struct ui_settings s;
	struct ui_contact c;
	int filter = lv_subject_get_int(mcui_contacts_filter);
	int n = ui_contacts_count();

	ui_settings_read(&s);
	self_known = s.lat_e6 != 0 || s.lon_e6 != 0;
	self_lat = s.lat_e6;
	self_lon = s.lon_e6;
	snprintf(self_name, sizeof(self_name), "%s", s.name[0] ? s.name : "You");

	node_count = 0;
	with_position = 0;
	for (int i = 0; i < n; i++) {
		if (!ui_contacts_get(i, &c) || (filter && c.type != filter) || (c.lat_e6 == 0 && c.lon_e6 == 0)) {
			continue;
		}
		with_position++;
		/* Keep the newest: insert by age, drop the oldest when full. */
		int pos = node_count;

		while (pos > 0 && nodes[pos - 1].age_s > c.age_s) {
			pos--;
		}
		if (pos >= MAP_NODES_MAX) {
			continue;
		}
		int last = node_count < MAP_NODES_MAX ? node_count : MAP_NODES_MAX - 1;

		memmove(&nodes[pos + 1], &nodes[pos], (size_t)(last - pos) * sizeof(nodes[0]));
		memcpy(nodes[pos].key, c.pub_key, UI_PUB_KEY_SIZE);
		snprintf(nodes[pos].name, sizeof(nodes[pos].name), "%s", c.name);
		nodes[pos].type = c.type;
		nodes[pos].lat = c.lat_e6;
		nodes[pos].lon = c.lon_e6;
		nodes[pos].age_s = c.age_s;
		if (node_count < MAP_NODES_MAX) {
			node_count++;
		}
	}
}

static void update_status(void)
{
	static const char *const what[] = {"nodes", "companions", "repeaters", "rooms", "sensors"};
	int filter = lv_subject_get_int(mcui_contacts_filter);
	char buf[64];

	if (!self_known && with_position == 0) {
		snprintf(buf, sizeof(buf), "No positions yet");
	} else if (with_position > node_count) {
		snprintf(buf, sizeof(buf), "Newest %d of %d %s", node_count, with_position, what[filter]);
	} else {
		snprintf(buf, sizeof(buf), "%d %s with a position%s", with_position, what[filter],
			 map_tiles_available() ? "" : " | no map tiles");
	}
	lv_subject_set_string(mcui_map_status, buf);
}

static void fit(void)
{
	double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9, x, y;
	int count = 0;

	for (int i = -1; i < node_count; i++) {
		if (i < 0 && !self_known) {
			continue;
		}
		map_project(i < 0 ? self_lat : nodes[i].lat, i < 0 ? self_lon : nodes[i].lon, 0, &x, &y);
		x0 = MIN(x0, x);
		y0 = MIN(y0, y);
		x1 = MAX(x1, x);
		y1 = MAX(y1, y);
		count++;
	}
	if (count == 0) {
		zoom = 2;
		cx = cy = 256.0 * (1 << zoom) / 2;
		return;
	}
	lv_obj_update_layout(map_area);
	/* Room for the markers' labels and the card. */
	zoom = map_fit_zoom(x0, y0, x1, y1, lv_obj_get_width(map_area) - 70, lv_obj_get_height(map_area) - 60);
	if (zoom > 15) {
		zoom = 15;
	}
	cx = (x0 + x1) / 2 * (1 << zoom);
	cy = (y0 + y1) / 2 * (1 << zoom);
}

/* Screen position of a position, relative to the map area's top left. */
static void to_screen(int32_t lat, int32_t lon, int w, int h, int32_t *sx, int32_t *sy)
{
	double x, y;

	map_project(lat, lon, zoom, &x, &y);
	*sx = (int32_t)lround(x - cx + w / 2.0);
	*sy = (int32_t)lround(y - cy + h / 2.0);
}

/* Text on a dark backing, readable over light map tiles too. */
static void draw_text(lv_layer_t *layer, const char *text, int32_t x, int32_t y, lv_color_t color)
{
	lv_draw_label_dsc_t d;
	lv_draw_rect_dsc_t bg;
	lv_point_t size;

	lv_text_get_size(&size, text, &mcui_font_small, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	int32_t tw = size.x;
	lv_area_t a = {x, y, x + tw, y + 15};
	lv_area_t ba = {x - 2, y - 1, x + tw + 2, y + 15};

	lv_draw_rect_dsc_init(&bg);
	bg.bg_color = MCUI_BG;
	bg.bg_opa = LV_OPA_60;
	bg.radius = 3;
	lv_draw_rect(layer, &bg, &ba);
	lv_draw_label_dsc_init(&d);
	d.text = text;
	d.text_local = 1;
	d.font = &mcui_font_small;
	d.color = color;
	lv_draw_label(layer, &d, &a);
}

static void draw_dot(lv_layer_t *layer, int32_t x, int32_t y, int r, lv_color_t fill, lv_color_t edge)
{
	lv_draw_rect_dsc_t d;
	lv_area_t a = {x - r, y - r, x + r, y + r};

	lv_draw_rect_dsc_init(&d);
	d.radius = LV_RADIUS_CIRCLE;
	d.bg_color = fill;
	d.bg_opa = LV_OPA_COVER;
	d.border_color = edge;
	d.border_width = 1;
	d.border_opa = LV_OPA_COVER;
	lv_draw_rect(layer, &d, &a);
}

/* Returns how many tiles it drew. */
static int draw_tiles(lv_layer_t *layer, const lv_area_t *a, int w, int h)
{
	double left = cx - w / 2.0, top = cy - h / 2.0;
	int n = 1 << zoom;
	int tx0 = (int)floor(left / MAP_TILE_SIZE), ty0 = (int)floor(top / MAP_TILE_SIZE);
	int tx1 = (int)floor((left + w - 1) / MAP_TILE_SIZE), ty1 = (int)floor((top + h - 1) / MAP_TILE_SIZE);

	int drawn = 0;

	map_tiles_keep(zoom, tx0, ty0, tx1, ty1);
	for (int ty = ty0; ty <= ty1; ty++) {
		for (int tx = tx0; tx <= tx1; tx++) {
			if (tx < 0 || ty < 0 || tx >= n || ty >= n) {
				continue;
			}
			const lv_image_dsc_t *tile = map_tile_get(zoom, tx, ty);

			if (!tile) {
				continue;
			}
			lv_draw_image_dsc_t d;
			int32_t x = a->x1 + (int32_t)lround(tx * (double)MAP_TILE_SIZE - left);
			int32_t y = a->y1 + (int32_t)lround(ty * (double)MAP_TILE_SIZE - top);
			lv_area_t ta = {x, y, x + MAP_TILE_SIZE - 1, y + MAP_TILE_SIZE - 1};

			lv_draw_image_dsc_init(&d);
			d.src = tile;
			lv_draw_image(layer, &d, &ta);
			drawn++;
		}
	}
	return drawn;
}

static void map_draw(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t a;
	char label[16];
	int32_t sx, sy;

	lv_obj_get_coords(map_area, &a);
	int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
	/* No tiles at this zoom (or none at all): rings instead. */
	bool tiles = map_tiles_available() && draw_tiles(layer, &a, w, h) > 0;

	int32_t center_lat = self_known ? self_lat : (node_count ? nodes[0].lat : 0);
	int scale_px = map_scale(center_lat, zoom, w / 3, label, sizeof(label));

	/* Without tiles: rings around this node, one scale-bar length apart. */
	if (!tiles && self_known && scale_px > 4) {
		lv_draw_arc_dsc_t d;

		to_screen(self_lat, self_lon, w, h, &sx, &sy);
		lv_draw_arc_dsc_init(&d);
		d.color = MCUI_MAP_LINE;
		d.width = 1;
		d.start_angle = 0;
		d.end_angle = 360;
		d.center.x = a.x1 + sx;
		d.center.y = a.y1 + sy;
		for (int k = 1; k <= 8 && scale_px * k < 2 * (w + h); k++) {
			d.radius = (uint16_t)(scale_px * k);
			lv_draw_arc(layer, &d);
		}
	}

	/* Contacts, oldest first so the newest end up on top. */
	int in_view = 0;

	for (int i = 0; i < node_count; i++) {
		to_screen(nodes[i].lat, nodes[i].lon, w, h, &sx, &sy);
		in_view += sx >= 0 && sy >= 0 && sx < w && sy < h;
	}
	for (int i = node_count - 1; i >= 0; i--) {
		to_screen(nodes[i].lat, nodes[i].lon, w, h, &sx, &sy);
		if (sx < -60 || sy < -20 || sx > w + 20 || sy > h + 20) {
			continue;
		}
		bool sel = i == selected;

		draw_dot(layer, a.x1 + sx, a.y1 + sy, sel ? MARKER_R + 3 : MARKER_R, type_color(nodes[i].type),
			 sel ? MCUI_TEXT : MCUI_BG);
		if (sel || in_view <= LABELS_MAX) {
			draw_text(layer, nodes[i].name, a.x1 + sx + MARKER_R + 3, a.y1 + sy - 8, MCUI_TEXT);
		}
	}
	if (self_known) {
		to_screen(self_lat, self_lon, w, h, &sx, &sy);
		draw_dot(layer, a.x1 + sx, a.y1 + sy, selected == SEL_SELF ? 9 : 7, MCUI_ACCENT, MCUI_TEXT);
		draw_text(layer, "You", a.x1 + sx + 10, a.y1 + sy - 8, MCUI_ACCENT);
	}

	/* Scale bar, bottom left, on a dark backing. */
	lv_draw_line_dsc_t ld;
	lv_draw_rect_dsc_t bg;
	int32_t bx = a.x1 + 8, by = a.y2 - 8;
	lv_area_t ba = {bx - 4, by - 22, bx + scale_px + 4, by + 4};

	lv_draw_rect_dsc_init(&bg);
	bg.bg_color = MCUI_BG;
	bg.bg_opa = LV_OPA_60;
	bg.radius = 3;
	lv_draw_rect(layer, &bg, &ba);

	lv_draw_line_dsc_init(&ld);
	ld.color = MCUI_TEXT;
	ld.width = 2;
	ld.p1.x = bx;
	ld.p1.y = by;
	ld.p2.x = bx + scale_px;
	ld.p2.y = by;
	lv_draw_line(layer, &ld);
	ld.p2.x = bx;
	ld.p2.y = by - 5;
	lv_draw_line(layer, &ld);
	ld.p1.x = ld.p2.x = bx + scale_px;
	lv_draw_line(layer, &ld);
	draw_text(layer, label, bx + 2, by - 21, MCUI_TEXT);

	if (!self_known && node_count == 0) {
		draw_text(layer, "Set your position in Settings >", a.x1 + 12, a.y1 + h / 2 - 20, MCUI_MUTED);
		draw_text(layer, "Public info. Nodes show up as their", a.x1 + 12, a.y1 + h / 2 - 4, MCUI_MUTED);
		draw_text(layer, "adverts bring a position.", a.x1 + 12, a.y1 + h / 2 + 12, MCUI_MUTED);
	}
}

/* ========== Selection ========== */

static void fmt_heard(char *out, size_t len, uint32_t age_s)
{
	if (age_s == UINT32_MAX) {
		snprintf(out, len, "not heard yet");
	} else if (age_s < 3600) {
		snprintf(out, len, "heard %um ago", (unsigned)(age_s / 60));
	} else if (age_s < 86400) {
		snprintf(out, len, "heard %uh ago", (unsigned)(age_s / 3600));
	} else {
		snprintf(out, len, "heard %ud ago", (unsigned)(age_s / 86400));
	}
}

/* Microdegrees as "-12.34567" */
static void fmt_deg(char *out, size_t len, int32_t e6)
{
	uint32_t a = e6 < 0 ? (uint32_t)(-(int64_t)e6) : (uint32_t)e6;

	snprintf(out, len, "%s%u.%05u", e6 < 0 ? "-" : "", (unsigned)(a / 1000000), (unsigned)(a % 1000000 / 10));
}

static void select_marker(int i)
{
	lv_obj_t *card = lv_obj_find_by_name(map_scr, "map_card");
	lv_obj_t *open = lv_obj_find_by_name(map_scr, "map_open");
	char info[64], dist[16], heard[24];

	selected = i;
	if (card) {
		lv_obj_set_hidden(card, i == SEL_NONE);
	}
	if (i == SEL_SELF) {
		char lat[16], lon[16];

		fmt_deg(lat, sizeof(lat), self_lat);
		fmt_deg(lon, sizeof(lon), self_lon);
		lv_subject_set_string(mcui_map_sel_name, self_name);
		snprintf(info, sizeof(info), "You | %s, %s", lat, lon);
		lv_subject_set_string(mcui_map_sel_info, info);
	} else if (i >= 0) {
		static const char *const kind[] = {"Node", "Companion", "Repeater", "Room", "Sensor"};
		const struct map_node *m = &nodes[i];

		fmt_heard(heard, sizeof(heard), m->age_s);
		if (self_known) {
			map_fmt_distance(map_distance_m(self_lat, self_lon, m->lat, m->lon), dist, sizeof(dist));
		} else {
			snprintf(dist, sizeof(dist), "?");
		}
		snprintf(info, sizeof(info), "%s | %s | %s", kind[m->type <= 4 ? m->type : 0], dist, heard);
		lv_subject_set_string(mcui_map_sel_name, m->name);
		lv_subject_set_string(mcui_map_sel_info, info);
		lv_subject_set_string(mcui_map_sel_action, m->type == UI_NODE_CHAT ? "Chat" : "Manage");
	}
	if (open) {
		lv_obj_set_hidden(open, i < 0);
	}
	lv_obj_invalidate(map_area);
}

static void hit_test(void)
{
	lv_point_t p;
	lv_area_t a;
	int best = SEL_NONE;
	int32_t best_d = HIT_PX * HIT_PX, sx, sy;

	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_get_coords(map_area, &a);
	int w = lv_area_get_width(&a), h = lv_area_get_height(&a);

	for (int i = -1; i < node_count; i++) {
		if (i < 0 && !self_known) {
			continue;
		}
		to_screen(i < 0 ? self_lat : nodes[i].lat, i < 0 ? self_lon : nodes[i].lon, w, h, &sx, &sy);
		int32_t dx = a.x1 + sx - p.x, dy = a.y1 + sy - p.y;

		if (dx * dx + dy * dy < best_d) {
			best_d = dx * dx + dy * dy;
			best = i;
		}
	}
	select_marker(best);
}

/* ========== Input ========== */

static void center_on(int i)
{
	double x, y;

	map_project(i < 0 ? self_lat : nodes[i].lat, i < 0 ? self_lon : nodes[i].lon, zoom, &x, &y);
	cx = x;
	cy = y;
}

static void pick_nearest_center(void)
{
	double bx = 0, by = 0, best = 1e300, x, y;
	int pick = SEL_NONE;

	for (int i = 0; i < node_count; i++) {
		map_project(nodes[i].lat, nodes[i].lon, zoom, &x, &y);
		double d = (x - cx) * (x - cx) + (y - cy) * (y - cy);

		if (d < best) {
			best = d;
			pick = i;
			bx = x;
			by = y;
		}
	}
	if (pick >= 0) {
		cx = bx;
		cy = by;
	}
	select_marker(pick);
}

void ui_lvgl_map_next(void)
{
	if (!map_area || node_count == 0) {
		return;
	}
	int next = selected < 0 ? 0 : (selected + 1) % node_count;

	center_on(next);
	select_marker(next);
}


bool ui_lvgl_map_back(void)
{
	if (!map_area || lv_screen_active() != map_scr || selected == SEL_NONE) {
		return false;
	}
	select_marker(SEL_NONE);
	return true;
}

static void map_input(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_PRESSED) {
		drag_px = 0;
	} else if (code == LV_EVENT_PRESSING) {
		lv_point_t v;

		lv_indev_get_vect(lv_indev_active(), &v);
		if (v.x || v.y) {
			double world = 256.0 * (1 << zoom);

			cx -= v.x;
			cy -= v.y;
			cy = cy < 0 ? 0 : (cy > world ? world : cy);
			drag_px += abs(v.x) + abs(v.y);
			lv_obj_invalidate(map_area);
		}
	} else if (code == LV_EVENT_CLICKED && lv_indev_get_type(lv_indev_active()) == LV_INDEV_TYPE_KEYPAD) {
		/* Centre key: open the node picked, or pick the one nearest the middle. */
		if (selected >= 0) {
			ui_lvgl_open_contact(nodes[selected].key);
		} else {
			pick_nearest_center();
		}
	} else if (code == LV_EVENT_CLICKED && drag_px < DRAG_PX) {
		hit_test();
	} else if (code == LV_EVENT_KEY) {
		uint32_t key = lv_event_get_key(e);
		double world = 256.0 * (1 << zoom);

		cx += key == LV_KEY_RIGHT ? KEY_PAN_PX : key == LV_KEY_LEFT ? -KEY_PAN_PX : 0;
		cy += key == LV_KEY_DOWN ? KEY_PAN_PX : key == LV_KEY_UP ? -KEY_PAN_PX : 0;
		cy = cy < 0 ? 0 : (cy > world ? world : cy);
		lv_obj_invalidate(map_area);
	}
}

static void map_screen_deleted(lv_event_t *e)
{
	ARG_UNUSED(e);
	map_scr = NULL;
	map_area = NULL;
}

void ui_on_map_open(lv_event_t *e)
{
	map_scr = lv_event_get_current_target_obj(e);
	map_area = lv_obj_find_by_name(map_scr, "map_area");
	if (!map_area) {
		return;
	}
	lv_obj_add_event_cb(map_scr, map_screen_deleted, LV_EVENT_DELETE, NULL);
	lv_obj_add_event_cb(map_area, map_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(map_area, map_input, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(map_area, map_input, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(map_area, map_input, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(map_area, map_input, LV_EVENT_KEY, NULL);
	lv_subject_set_string(mcui_map_attrib, map_tiles_available() ? map_tiles_attribution() : "");
	load_nodes();
	update_status();
	fit();
	select_marker(SEL_NONE);
}

static void zoom_cmd(const char *how)
{
	if (!map_area || !how) {
		return;
	}
	if (strcmp(how, "in") == 0 && zoom < MAP_ZOOM_MAX) {
		zoom++;
		cx *= 2;
		cy *= 2;
	} else if (strcmp(how, "out") == 0 && zoom > MAP_ZOOM_MIN) {
		zoom--;
		cx /= 2;
		cy /= 2;
	} else if (strcmp(how, "fit") == 0) {
		fit();
	}
	lv_obj_invalidate(map_area);
}

void ui_on_map_zoom(lv_event_t *e)
{
	zoom_cmd(lv_event_get_user_data(e));
}

void ui_lvgl_map_zoom(bool in)
{
	zoom_cmd(in ? "in" : "out");
}

void ui_on_map_open_node(lv_event_t *e)
{
	ARG_UNUSED(e);
	if (selected >= 0 && selected < node_count) {
		ui_lvgl_open_contact(nodes[selected].key);
	}
}

void ui_lvgl_map_refresh(void)
{
	if (!map_area) {
		return;
	}
	/* The selection is by index: keep it only while it still names the same node. */
	uint8_t sel_key[UI_PUB_KEY_SIZE];
	bool had = selected >= 0;

	if (had) {
		memcpy(sel_key, nodes[selected].key, UI_PUB_KEY_SIZE);
	}
	load_nodes();
	update_status();
	if (had) {
		int found = SEL_NONE;

		for (int i = 0; i < node_count; i++) {
			if (memcmp(nodes[i].key, sel_key, UI_PUB_KEY_SIZE) == 0) {
				found = i;
			}
		}
		select_marker(found);
	}
	lv_obj_invalidate(map_area);
}
