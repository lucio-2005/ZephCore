#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Convert slippy-map tiles for the LVGL touch UI's map.

Reads raster tiles from a folder laid out as <z>/<x>/<y>.png (or .jpg, .webp,
anything Pillow opens), or from an MBTiles file, and writes them as
<z>/<x>/<y>.bin in LVGL's binary image format, RGB565, which the UI draws
without decoding. Tiles are 256 x 256; 512 x 512 ("@2x") tiles are scaled down.

    python3 map_tiles.py SRC DST [--min-zoom 8] [--max-zoom 15] [--attribution TEXT]

SRC is a folder or a .mbtiles file. An MBTiles file's own attribution is used
when --attribution is not given. Vector MBTiles (pbf) must be rendered to
raster first.

Tiles are not bundled and must not be bulk-downloaded from
tile.openstreetmap.org (its usage policy forbids it). Render your own, or use
a provider whose terms allow offline use, and keep its attribution: it is
written to DST/attribution.txt and shown in the corner of the map.

Then run the desktop preview from the folder that holds DST/ as "tiles", or
point ZEPHCORE_MAP_TILES at it.
"""

import argparse
import array
import io
import os
import re
import sqlite3
import struct
import sys
import unicodedata

try:
    from PIL import Image
except ImportError:
    sys.exit("map_tiles.py needs Pillow: pip install Pillow")

TILE = 256
LV_IMAGE_HEADER_MAGIC = 0x19
LV_COLOR_FORMAT_RGB565 = 0x12


def to_lvgl_bin(img):
    """LVGL v9 image: 12-byte header (magic, cf, flags, w, h, stride), then RGB565 rows."""
    img = img.convert("RGB")
    stride = TILE * 2
    header = struct.pack("<BBHHHHH", LV_IMAGE_HEADER_MAGIC, LV_COLOR_FORMAT_RGB565, 0,
                         TILE, TILE, stride, 0)
    raw = img.tobytes()
    px = array.array("H", (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                           for r, g, b in zip(raw[0::3], raw[1::3], raw[2::3])))
    if sys.byteorder != "little":
        px.byteswap()
    return header + px.tobytes()


def folder_tiles(src, zmin, zmax):
    """(z, x, y, file object) for each tile in <z>/<x>/<y>.<ext>."""
    for z_name in sorted(os.listdir(src)):
        if not z_name.isdigit() or not zmin <= int(z_name) <= zmax:
            continue
        z_dir = os.path.join(src, z_name)
        for x_name in sorted(os.listdir(z_dir)):
            x_dir = os.path.join(z_dir, x_name)
            if not x_name.isdigit() or not os.path.isdir(x_dir):
                continue
            for f in sorted(os.listdir(x_dir)):
                y_name, _ = os.path.splitext(f)
                if y_name.isdigit():
                    yield int(z_name), int(x_name), int(y_name), open(os.path.join(x_dir, f), "rb")


def mbtiles_metadata(db):
    try:
        return dict(db.execute("SELECT name, value FROM metadata"))
    except sqlite3.DatabaseError:
        return {}


def mbtiles_tiles(db, zmin, zmax):
    """(z, x, y, file object); MBTiles counts rows from the south (TMS), so y is flipped."""
    rows = db.execute("SELECT zoom_level, tile_column, tile_row, tile_data FROM tiles "
                      "WHERE zoom_level BETWEEN ? AND ? ORDER BY zoom_level, tile_column, tile_row",
                      (zmin, zmax))
    for z, x, row, data in rows:
        yield z, x, (1 << z) - 1 - row, io.BytesIO(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("src", help="folder with <z>/<x>/<y>.<image> tiles, or an .mbtiles file")
    ap.add_argument("dst", help="output folder")
    ap.add_argument("--min-zoom", type=int, default=0)
    ap.add_argument("--max-zoom", type=int, default=19)
    ap.add_argument("--attribution", help="text for the map corner, e.g. the tile source's credit")
    args = ap.parse_args()

    attribution = args.attribution
    if os.path.isdir(args.src):
        tiles = folder_tiles(args.src, args.min_zoom, args.max_zoom)
    elif os.path.isfile(args.src):
        db = sqlite3.connect(f"file:{args.src}?mode=ro", uri=True)
        meta = mbtiles_metadata(db)
        if meta.get("format", "").lower() in ("pbf", "mvt"):
            sys.exit(f"{args.src} holds vector tiles: render them to raster (png/jpg) first")
        if not attribution and meta.get("attribution"):
            attribution = re.sub(r"<[^>]*>", "", meta["attribution"]).replace("&copy;", "(c)").replace("&amp;", "&")
        tiles = mbtiles_tiles(db, args.min_zoom, args.max_zoom)
    else:
        sys.exit(f"{args.src}: no such folder or file")

    done = skipped = 0
    for z, x, y, f in tiles:
        with f, Image.open(f) as img:
            if img.size == (2 * TILE, 2 * TILE):
                img = img.convert("RGB").resize((TILE, TILE), Image.LANCZOS)
            elif img.size != (TILE, TILE):
                print(f"skipped {z}/{x}/{y}: {img.size[0]}x{img.size[1]}, not 256x256 or 512x512")
                skipped += 1
                continue
            data = to_lvgl_bin(img)
        out_dir = os.path.join(args.dst, str(z), str(x))
        os.makedirs(out_dir, exist_ok=True)
        with open(os.path.join(out_dir, f"{y}.bin"), "wb") as out:
            out.write(data)
        done += 1

    if attribution:
        # The UI's fonts are ASCII only.
        line = attribution.strip().splitlines()[0].replace("\u00a9", "(c)")
        line = unicodedata.normalize("NFKD", line).encode("ascii", "ignore").decode()
        os.makedirs(args.dst, exist_ok=True)
        with open(os.path.join(args.dst, "attribution.txt"), "w") as out:
            out.write(line + "\n")
    print(f"{done} tiles written to {args.dst}" + (f", {skipped} skipped" if skipped else ""))
    return 0 if done else 1


if __name__ == "__main__":
    sys.exit(main())
