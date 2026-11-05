#!/usr/bin/env python3
"""Write Grove's icon as a Windows .ico or a C array of PNG bytes.

The mark matches ui/window_icon.c: a paper disc, a brown rim, a canopy,
one lighter leaf, and a trunk. Windows embeds the .ico in the program.
macOS reads the PNG bytes.
"""

import argparse
import struct
import sys
import zlib


def in_disc(px, py, cx, cy, r):
    dx = px - cx
    dy = py - cy
    return dx * dx + dy * dy <= r * r


def icon_sample(px, py):
    if not in_disc(px, py, 500, 500, 460):
        return 0
    rgb = 0x00F4F1EA
    if not in_disc(px, py, 500, 500, 400):
        rgb = 0x008A4E22
    if in_disc(px, py, 500, 400, 240):
        rgb = 0x0035543C
    if in_disc(px, py, 400, 360, 100):
        rgb = 0x004E7052
    if 440 <= px < 560 and 500 <= py < 800:
        rgb = 0x008A4E22
    return 0xFF000000 | rgb


def sample_pos(side, pixel, sample):
    num = (2 * (pixel * 4 + sample) + 1) * 1000
    den = 2 * side * 4
    if den <= 0:
        return 0
    return num // den


def pixel_at(side, x, y):
    sum_r = 0
    sum_g = 0
    sum_b = 0
    opaque = 0
    for sy in range(4):
        py = sample_pos(side, y, sy)
        for sx in range(4):
            sample = icon_sample(sample_pos(side, x, sx), py)
            if sample == 0:
                continue
            opaque += 1
            sum_r += (sample >> 16) & 255
            sum_g += (sample >> 8) & 255
            sum_b += sample & 255
    if opaque <= 0:
        return (0, 0, 0, 0)
    return (opaque * 255 // 16, sum_r // opaque, sum_g // opaque, sum_b // opaque)


def png_bytes(side):
    raw = bytearray()
    for y in range(side):
        raw.append(0)
        for x in range(side):
            a, r, g, b = pixel_at(side, x, y)
            raw += bytes((r, g, b, a))
    comp = zlib.compress(bytes(raw), 9)

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", side, side, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", comp) + chunk(b"IEND", b"")


def dib_bytes(side):
    xor = bytearray()
    for y in range(side - 1, -1, -1):
        for x in range(side):
            a, r, g, b = pixel_at(side, x, y)
            xor += bytes((b, g, r, a))
    stride = ((side + 31) // 32) * 4
    mask = bytes(stride * side)
    hdr = struct.pack("<IiiHHIIiiII", 40, side, side * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
    return hdr + bytes(xor) + mask


def ico_bytes():
    sides = (16, 32, 48, 256)
    blobs = [dib_bytes(side) for side in sides]
    header = struct.pack("<HHH", 0, 1, len(sides))
    offset = 6 + 16 * len(sides)
    entries = bytearray()
    for side, blob in zip(sides, blobs):
        dim = 0 if side >= 256 else side
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    return header + bytes(entries) + b"".join(blobs)


def header_text():
    png = png_bytes(128)
    lines = ["#ifndef GROVE_ICON_PNG_H", "#define GROVE_ICON_PNG_H", "", "static const unsigned char grove_icon_png[] = {"]
    for i in range(0, len(png), 16):
        chunk = png[i : i + 16]
        lines.append("    " + ", ".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};")
    lines.append("static const unsigned int grove_icon_png_len = %d;" % len(png))
    lines.append("")
    lines.append("#endif")
    lines.append("")
    return "\n".join(lines)


def main(argv):
    parser = argparse.ArgumentParser(description="Write Grove's icon.")
    parser.add_argument("--ico", help="Windows icon path")
    parser.add_argument("--header", help="C header path for the PNG bytes")
    args = parser.parse_args(argv)
    if not args.ico and not args.header:
        parser.error("pass --ico or --header")
    if args.ico:
        with open(args.ico, "wb") as f:
            f.write(ico_bytes())
    if args.header:
        with open(args.header, "w", encoding="utf-8", newline="\n") as f:
            f.write(header_text())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
