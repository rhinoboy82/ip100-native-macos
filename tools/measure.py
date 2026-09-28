#!/usr/bin/env python3
"""Measure ink amounts in a Canon iP100 capture of test/color_test.pdf.

Usage: measure.py CAPTURE.prn TEST_IMAGE.ppm OUT_PREFIX
Writes OUT_PREFIX_model.json (hue x lightness grid + grey axis), OUT_PREFIX_blend.json and
OUT_PREFIX_samples.json, the inputs for tools/buildlut.py.

TEST_IMAGE.ppm is the 150 dpi source image of color_test.pdf (1275 x 1650). Coordinates below
are in that image's pixels; the capture's resolution (600 or 300 dpi) is read from its ESC (d.
Ink amounts are in drops per dot at the capture's resolution; for multi-level modes (photo
papers, Super Fine, envelope) they are the mean ink level per dot (C/M 0-5, Y 0-3, k 0-3, K 0-1).
"""
import colorsys
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from colordecode import decode  # noqa: E402
from photolevels import LevelPage  # noqa: E402

T = [[(b // 81) % 3, (b // 27) % 3, (b // 9) % 3, (b // 3) % 3, b % 3] for b in range(256)]


class Capture:
    def __init__(self, path):
        self.d = decode(path)
        self.inks = [chr(c) for c in self.d["inks"]]
        self.ternary = self.d["t"][3:4] == b"\x22"  # C/M 3-level packing in 600 dpi standard mode
        self.scale = self.d["res"][0] / 150  # capture dots per source pixel
        self.left, self.top = 151 * self.d["res"][0] // 600, 70 * self.d["res"][1] // 600
        self.cache = {}

    def row(self, ch, y):
        key = (ch, y)
        if key not in self.cache:
            rows = self.d["channels"][ch]
            r = rows[y] if 0 <= y < len(rows) else b""
            if self.ternary and self.inks[ch] in "CM":
                self.cache[key] = [v for b in r for v in T[b]]
            else:
                self.cache[key] = [(b >> (7 - q)) & 1 for b in r for q in range(8)]
        return self.cache[key]

    def amount(self, x0, y0, x1, y1):
        """Mean ink per dot for each channel over a rectangle in source-image pixels."""
        s = self.scale
        X0, X1 = int(s * x0) - self.left, int(s * x1) - self.left
        Y0, Y1 = int(s * y0) - self.top, int(s * y1) - self.top
        step = max(1, int(s // 2))
        out = []
        for ch in range(len(self.inks)):
            tot = n = 0
            for y in range(Y0, Y1, step):
                seg = self.row(ch, y)[X0:X1:step]
                tot += sum(seg)
                n += len(range(X0, X1, step))
            out.append(tot / n if n else 0.0)
        return out


class LevelCapture:
    """Same interface as Capture, for multi-level captures (split C/M/k channels)."""

    def __init__(self, path):
        self.p = LevelPage(path)
        self.d = self.p.d
        self.inks = self.p.inks  # C, M, Y, k|K

    def amount(self, x0, y0, x1, y1):
        X0, X1, Y0, Y1 = 4 * x0 - 151, 4 * x1 - 151, 4 * y0 - 70, 4 * y1 - 70
        out = []
        for ink in self.inks:
            tot = n = 0
            for y in range(Y0, Y1, 2):
                row = self.p.levels(ink, y)
                seg = row[X0:X1:2]
                tot += sum(max(v, 0) for v in seg)
                n += len(range(X0, X1, 2))
            out.append(tot / n if n else 0.0)
        return out


def main():
    d0 = decode(sys.argv[1])
    cap = LevelCapture(sys.argv[1]) if 0x83 in d0["inks"] else Capture(sys.argv[1])
    raw = open(sys.argv[2], "rb").read()
    px = raw[raw.index(b"255\n") + 4:]
    W = 1275

    def rgb_avg(x, y, r=2):
        pts = [(x + dx, y + dy) for dx in range(-r, r + 1, 2) for dy in range(-r, r + 1, 2)]
        return [sum(px[(py * W + qx) * 3 + k] for qx, py in pts) / len(pts) for k in range(3)]

    prefix = sys.argv[3]
    # Fixed patches and grey ramp
    pure = [(0, 255, 255), (255, 0, 255), (255, 255, 0), (0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255)]
    half = [(128, 255, 255), (255, 128, 255), (255, 255, 128), (128, 128, 128), (64, 64, 64), (192, 192, 192), (160, 110, 70)]
    samples = []
    for cols, y0 in ((pure, 90), (half, 330)):
        for i, c in enumerate(cols):
            x0 = 90 + i * 155 + 20
            samples.append([list(c), cap.amount(x0, y0 + 20, x0 + 100, y0 + 190)])
    grey = {0: samples[3][1], 255: [0.0] * 4}
    for g in range(16, 255, 16):
        x = 90 + int(g / 255 * 1094)
        a = cap.amount(x - 4, 600, x + 4, 700)
        samples.append([[g, g, g], a])
        grey[g] = a
    json.dump(samples, open(prefix + "_samples.json", "w"))

    # Hue x lightness gradient (x 90..1185, y 760..1160)
    NH, NL, GW, GH = 36, 16, 1095, 400
    grid = []
    for i in range(NH):
        col = []
        for j in range(NL):
            cx, cy = int((i + 0.5) / NH * GW), int((j + 0.5) / NL * GH)
            r, g, b = rgb_avg(90 + cx, 760 + cy)
            h, l, s = colorsys.rgb_to_hls(r / 255, g / 255, b / 255)
            col.append(dict(rgb=(r, g, b), h=h, l=l, s=s, ink=cap.amount(90 + cx - 5, 760 + cy - 5, 90 + cx + 5, 760 + cy + 5)))
        grid.append(col)
    json.dump(dict(grid=grid, grey=sorted(grey.items())), open(prefix + "_model.json", "w"))

    # Blend region (x 90..1185, y 1200..1560)
    blend = []
    for i in range(12):
        for j in range(6):
            x, y = 150 + i * 85, 1230 + j * 55
            blend.append([[round(v) for v in rgb_avg(x, y, 3)], cap.amount(x - 5, y - 5, x + 5, y + 5)])
    json.dump(blend, open(prefix + "_blend.json", "w"))
    print(f"{prefix}: {len(samples)} samples, {NH * NL} grid points, {len(blend)} blend points; "
          f"inks {''.join(cap.inks)} at {cap.d['res'][0]} dpi{' (multi-level)' if isinstance(cap, LevelCapture) else ''}")


if __name__ == "__main__":
    main()
