#!/usr/bin/env python3
"""Compare per-patch ink amounts of two iP100 colour streams of ref/color_test.pdf."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from colordecode import decode
T = [[(b // 81) % 3, (b // 27) % 3, (b // 9) % 3, (b // 3) % 3, b % 3] for b in range(256)]


class Page:
    def __init__(self, path):
        self.d = decode(path)
        self.inks = [chr(c) for c in self.d["inks"]]
        self.cache = {}

    def row(self, ch, y):
        k = (ch, y)
        if k not in self.cache:
            rows = self.d["channels"][ch]
            r = rows[y] if y < len(rows) else b""
            self.cache[k] = [v for b in r for v in T[b]] if self.inks[ch] in "CM" else [(b >> (7 - q)) & 1 for b in r for q in range(8)]
        return self.cache[k]

    def amount(self, x0, y0, x1, y1):
        out = []
        for ch in range(len(self.inks)):
            X0, X1 = 4 * x0 - 151, 4 * x1 - 151
            tot = n = 0
            for y in range(4 * y0 - 70, 4 * y1 - 70, 2):
                seg = self.row(ch, y)[X0:X1:2]
                tot += sum(seg)
                n += (X1 - X0 + 1) // 2
            out.append(tot / n)
        return out


a, b = Page(sys.argv[1]), Page(sys.argv[2])
pure = [(0, 255, 255), (255, 0, 255), (255, 255, 0), (0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255)]
half = [(128, 255, 255), (255, 128, 255), (255, 255, 128), (128, 128, 128), (64, 64, 64), (192, 192, 192), (160, 110, 70)]
tot = 0
print(f"{'colour':16} {'Canon C/M/Y/K':28} {'ours C/M/Y/K':28}")
for cols, y0 in ((pure, 90), (half, 330)):
    for i, c in enumerate(cols):
        x0 = 90 + i * 155 + 20
        pa, pb = a.amount(x0, y0 + 20, x0 + 100, y0 + 190), b.amount(x0, y0 + 20, x0 + 100, y0 + 190)
        tot += sum(abs(p - q) for p, q in zip(pa, pb))
        print(f"{str(c):16} {str([round(v, 2) for v in pa]):28} {str([round(v, 2) for v in pb]):28}")
for g in (32, 96, 160, 224):
    x = 90 + int(g / 255 * 1094)
    pa, pb = a.amount(x - 4, 600, x + 4, 700), b.amount(x - 4, 600, x + 4, 700)
    print(f"grey {g:<11} {str([round(v, 2) for v in pa]):28} {str([round(v, 2) for v in pb]):28}")
print(f"mean abs difference per patch (sum of 4 inks): {tot / 14:.3f}")
