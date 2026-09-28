#!/usr/bin/env python3
"""Per-dot ink levels for Canon iP100 multi-level modes (photo papers, Super Fine, envelope).

C and M are split over three channels (main 2-bit + two 1-bit planes); dye black k over two
(main ternary + one 1-bit plane). Level codes observed in Canon's output:
  C/M: 1=(0,0,1) 2=(0,1,1) 3=(1,1,0) 4=(2,1,0) 5=(3,1,0)   as (main, 0x40 plane, 0x80 plane)
  k:   1=(0,1) 2=(1,1) 3=(2,1)                             as (main, 0x40 plane)
  Y:   2-bit value 0..3;  K (pigment): 1 bit
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from colordecode import decode  # noqa: E402

T3 = [[(b // 81) % 3, (b // 27) % 3, (b // 9) % 3, (b // 3) % 3, b % 3] for b in range(256)]
CM_CODE = {(0, 0, 0): 0, (0, 0, 1): 1, (0, 1, 1): 2, (1, 1, 0): 3, (2, 1, 0): 4, (3, 1, 0): 5}
K_CODE = {(0, 0): 0, (0, 1): 1, (1, 1): 2, (2, 1): 3}


def bits2(row):
    return [(b >> s) & 3 for b in row for s in (6, 4, 2, 0)]


def bits1(row):
    return [(b >> (7 - q)) & 1 for b in row for q in range(8)]


def tern(row):
    return [v for b in row for v in T3[b]]


class LevelPage:
    """Decoded page giving per-dot levels for each logical ink (C, M, Y, K or k)."""

    def __init__(self, path, page=0):
        self.d = d = decode(path, page)
        self.idx = {ink: i for i, ink in enumerate(d["inks"])}
        self.inks = [c for c in "CMY" if ord(c) in self.idx] + (["k"] if 0x6B in self.idx else ["K"] if 0x4B in self.idx else [])
        self.cache = {}

    def _row(self, code, y, kind):
        ch = self.d["channels"][self.idx[code]]
        r = ch[y] if 0 <= y < len(ch) else b""
        return {1: bits1, 2: bits2, 3: tern}[kind](r)

    def levels(self, ink, y):
        key = (ink, y)
        if key in self.cache:
            return self.cache[key]
        if ink in "CM":
            base = ord(ink)
            a, b, c = self._row(base, y, 2), self._row(base + 0x40, y, 1), self._row(base + 0x80, y, 1)
            n = max(len(a), len(b), len(c))
            g = lambda v, i: v[i] if i < len(v) else 0
            out = [CM_CODE.get((g(a, i), g(b, i), g(c, i)), -1) for i in range(n)]
        elif ink == "Y":
            out = self._row(0x59, y, 2)
        elif ink == "k":
            a, b = self._row(0x6B, y, 3), self._row(0xAB, y, 1)
            n = max(len(a), len(b))
            g = lambda v, i: v[i] if i < len(v) else 0
            out = [K_CODE.get((g(a, i), g(b, i)), -1) for i in range(n)]
        else:
            out = self._row(0x4B, y, 1)
        self.cache[key] = out
        return out


if __name__ == "__main__":
    from collections import Counter
    p = LevelPage(sys.argv[1])
    # region in 150-dpi source pixels: x0 y0 x1 y1
    x0, y0, x1, y1 = map(int, sys.argv[2:6])
    for ink in p.inks:
        cnt = Counter()
        for y in range(4 * y0 - 70, 4 * y1 - 70, 2):
            row = p.levels(ink, y)
            for x in range(4 * x0 - 151, 4 * x1 - 151, 2):
                cnt[row[x] if x < len(row) else 0] += 1
        tot = sum(cnt.values())
        print(f"  {ink}: " + "  ".join(f"L{k}:{v * 100 / tot:4.1f}%" for k, v in sorted(cnt.items())))
