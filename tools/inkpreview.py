#!/usr/bin/env python3
"""Render a rough on-screen preview of a CMYK iP100 page (ink coverage averaged over 12x12 dots)."""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from colordecode import decode
T = [[(b // 81) % 3, (b // 27) % 3, (b // 9) % 3, (b // 3) % 3, b % 3] for b in range(256)]
d = decode(sys.argv[1]); inks = [chr(c) for c in d["inks"]]; S = 12
chans = d["channels"]; H = max(len(c) for c in chans); W = 4800
img = bytearray(b"\xff" * ((W // S) * (H // S) * 3))
for by in range(H // S):
    acc = [[0.0] * (W // S) for _ in inks]
    for y in range(by * S, by * S + S, 2):
        for ci, ink in enumerate(inks):
            row = chans[ci][y] if y < len(chans[ci]) else b""
            px = [v for b in row for v in T[b]] if ink in "CM" else [(b >> (7 - q)) & 1 for b in row for q in range(8)]
            for x in range(0, min(len(px), W), 2):
                acc[ci][x // S] += px[x]
    for bx in range(W // S):
        cov = {ink: acc[ci][bx] / (S * S / 4) for ci, ink in enumerate(inks)}
        # crude subtractive mix; C/M amounts are in drops (up to 2), scale so ~1 drop = full
        c = min(1, cov.get("C", 0) / 1.0); m = min(1, cov.get("M", 0) / 1.0); yv = min(1, cov.get("Y", 0) / 0.55); k = min(1, cov.get("K", 0))
        r = 255 * (1 - c) * (1 - k); g = 255 * (1 - m) * (1 - k); b = 255 * (1 - yv) * (1 - k)
        o = (by * (W // S) + bx) * 3; img[o:o + 3] = bytes((int(r), int(g), int(b)))
open(sys.argv[2], "wb").write(b"P6\n%d %d\n255\n" % (W // S, H // S) + img)
