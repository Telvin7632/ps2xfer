#!/usr/bin/env python3
"""Draws the ps2xfer icon (no dependencies): a blue rounded square with a white
arrow pointing into a disk. Writes pc/ps2xfer.ico (16..256 px, PNG entries) and
prints the 64 px PNG as base64 for the window icon in pc/ps2xfer.py.

    python3 tools/make_icon.py
"""
import base64
import os
import struct
import zlib

TOP, BOTTOM = (0x3B, 0x82, 0xF6), (0x1D, 0x4E, 0xD8)
WHITE, LED = (0xFF, 0xFF, 0xFF), (0x1D, 0x4E, 0xD8)


def in_round_rect(x, y, x0, y0, x1, y1, r):
    if not (x0 <= x <= x1 and y0 <= y <= y1):
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def in_triangle(x, y, a, b, c):
    def side(p, q):
        return (x - q[0]) * (p[1] - q[1]) - (p[0] - q[0]) * (y - q[1])
    d1, d2, d3 = side(a, b), side(b, c), side(c, a)
    neg = d1 < 0 or d2 < 0 or d3 < 0
    pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (neg and pos)


def sample(x, y):
    """Colour and coverage at a point of the 256x256 design."""
    if not in_round_rect(x, y, 8, 8, 248, 248, 52):
        return None
    if (in_round_rect(x, y, 110, 40, 146, 132, 6) or
            in_triangle(x, y, (64, 118), (192, 118), (128, 180))):
        return WHITE
    if in_round_rect(x, y, 52, 192, 204, 226, 14):
        if (x - 184) ** 2 + (y - 209) ** 2 <= 49:
            return LED
        return WHITE
    t = (y - 8) / 240
    return tuple(round(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3))


def render(size, ss=4):
    rows = []
    step = 256 / size
    for py in range(size):
        row = bytearray()
        for px in range(size):
            acc = [0, 0, 0]
            hits = 0
            for sy in range(ss):
                for sx in range(ss):
                    c = sample((px + (sx + 0.5) / ss) * step, (py + (sy + 0.5) / ss) * step)
                    if c is not None:
                        hits += 1
                        for i in range(3):
                            acc[i] += c[i]
            if hits:
                row += bytes((acc[0] // hits, acc[1] // hits, acc[2] // hits, 255 * hits // (ss * ss)))
            else:
                row += b"\0\0\0\0"
        rows.append(bytes(row))
    return rows


def png(rows):
    size = len(rows)
    raw = b"".join(b"\0" + r for r in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico(pngs):
    out = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    body = b""
    for size, data in pngs:
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset + len(body))
        body += data
    return out + body


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    pngs = [(s, png(render(s))) for s in (256, 64, 48, 32, 16)]
    with open(os.path.join(here, "..", "pc", "ps2xfer.ico"), "wb") as f:
        f.write(ico(pngs))
    print(base64.b64encode(dict(pngs)[64]).decode())


if __name__ == "__main__":
    main()
