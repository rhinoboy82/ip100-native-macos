#!/usr/bin/env python3
"""Decode a Canon PIXMA print stream into a readable command list.

Usage: canondump.py FILE [--all]
Raster (ESC (A / ESC (F) data blocks are summarised, not printed, unless --all.
"""
import sys


def dump(path, show_all=False):
    data = open(path, "rb").read()
    i, n = 0, len(data)
    raster = {}
    out = []
    while i < n:
        if data[i] == 0x1B and i + 1 < n and data[i + 1] in (0x28, 0x5B):
            kind = chr(data[i + 1])
            cmd = chr(data[i + 2])
            ln = data[i + 3] | (data[i + 4] << 8)
            body = data[i + 5:i + 5 + ln]
            name = f"ESC {kind}{cmd}"
            if cmd == "K" and b"BJLSTART" in body:
                out.append(f"{i:08x} {name} len={ln} {body.decode('latin1').strip()!r}")
            elif kind == "(" and cmd in "AF" and not show_all:
                key = (name, body[:1].hex() if cmd == "A" else "")
                raster[key] = raster.get(key, 0) + 1
                if raster[key] == 1:
                    out.append(f"{i:08x} {name} len={ln} (raster data, first of run) {body[:8].hex()}")
            else:
                out.append(f"{i:08x} {name} len={ln} {body.hex()}")
            i += 5 + ln
        elif data[i] == 0x0C:
            out.append(f"{i:08x} FF (form feed)")
            i += 1
        elif data[i] == 0x0D:
            out.append(f"{i:08x} CR")
            i += 1
        else:
            out.append(f"{i:08x} raw byte {data[i]:02x}")
            i += 1
    for line in out:
        print(line)
    print("raster block counts:", {f"{k[0]}{(' ' + k[1]) if k[1] else ''}": v for k, v in raster.items()})


if __name__ == "__main__":
    dump(sys.argv[1], "--all" in sys.argv)
