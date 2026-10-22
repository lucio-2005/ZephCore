#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Convert slippy-map tiles for the LVGL touch UI's map.

Reads a folder of 256 x 256 tiles laid out as <z>/<x>/<y>.png (or .jpg, .webp,
anything Pillow opens) and writes the same layout as <z>/<x>/<y>.bin in LVGL's
binary image format, RGB565, which the UI draws without decoding.

    python3 map_tiles.py SRC DST [--min-zoom 8] [--max-zoom 15] [--attribution TEXT]

Tiles are not bundled and must not be bulk-downloaded from
tile.openstreetmap.org (its usage policy forbids it). Render your own, or use
a provider whose terms allow offline use, and keep its attribution: it is
written to DST/attribution.txt and shown in the corner of the map.

Then run the desktop preview from the folder that holds DST/ as "tiles", or
point ZEPHCORE_MAP_TILES at it.
"""

import argparse
import array
import os
import struct
import sys

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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("src", help="folder with <z>/<x>/<y>.<image> tiles")
    ap.add_argument("dst", help="output folder")
    ap.add_argument("--min-zoom", type=int, default=0)
    ap.add_argument("--max-zoom", type=int, default=19)
    ap.add_argument("--attribution", help="text for the map corner, e.g. the tile source's credit")
    args = ap.parse_args()

    done = skipped = 0
    for z_name in sorted(os.listdir(args.src)):
        if not z_name.isdigit() or not args.min_zoom <= int(z_name) <= args.max_zoom:
            continue
        z_dir = os.path.join(args.src, z_name)
        for x_name in sorted(os.listdir(z_dir)):
            x_dir = os.path.join(z_dir, x_name)
            if not x_name.isdigit() or not os.path.isdir(x_dir):
                continue
            for f in sorted(os.listdir(x_dir)):
                y_name, _ = os.path.splitext(f)
                if not y_name.isdigit():
                    continue
                with Image.open(os.path.join(x_dir, f)) as img:
                    if img.size != (TILE, TILE):
                        print(f"skipped {z_name}/{x_name}/{f}: {img.size[0]}x{img.size[1]}, not 256x256")
                        skipped += 1
                        continue
                    data = to_lvgl_bin(img)
                out_dir = os.path.join(args.dst, z_name, x_name)
                os.makedirs(out_dir, exist_ok=True)
                with open(os.path.join(out_dir, y_name + ".bin"), "wb") as out:
                    out.write(data)
                done += 1

    if args.attribution:
        os.makedirs(args.dst, exist_ok=True)
        with open(os.path.join(args.dst, "attribution.txt"), "w") as out:
            out.write(args.attribution.strip() + "\n")
    print(f"{done} tiles written to {args.dst}" + (f", {skipped} skipped" if skipped else ""))
    return 0 if done else 1


if __name__ == "__main__":
    sys.exit(main())
