#!/usr/bin/env python3
"""Decode single-channel (ESC (L 'K') Canon multiraster data into a PBM image.

Assumes: ESC (J gives lines per ESC (F block, each line is PackBits data
terminated by 0x80, and ESC (e <n16be> skips n blocks.
Usage: rasterdecode.py IN.prn OUT.pbm [bits_per_pixel]
"""
import sys


def unpackbits(buf, i):
    """Decode one line of PackBits starting at buf[i]; stop at 0x80 terminator."""
    out = bytearray()
    while i < len(buf):
        c = buf[i]
        if c == 0x80:
            return out, i + 1
        if c < 0x80:
            out += buf[i + 1:i + 2 + c]
            i += 2 + c
        else:
            out += bytes([buf[i + 1]]) * (257 - c)
            i += 2
    return out, i


def main():
    data = open(sys.argv[1], "rb").read()
    bpp = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    lines_per_block, rows, i = 16, [], 0
    stats = {"blocks": 0, "skips": 0, "linelens": {}}
    while i < len(data):
        if data[i] == 0x1B and data[i + 1] in (0x28, 0x5B):
            cmd, ln = chr(data[i + 2]), data[i + 3] | data[i + 4] << 8
            body = data[i + 5:i + 5 + ln]
            if data[i + 1] == 0x28 and cmd == "J":
                lines_per_block = body[0]
            elif data[i + 1] == 0x28 and cmd == "e":
                n = int.from_bytes(body, "big")
                rows += [b""] * (n * lines_per_block)
                stats["skips"] += n
            elif data[i + 1] == 0x28 and cmd == "F":
                stats["blocks"] += 1
                j = 0
                for _ in range(lines_per_block):
                    line, j = unpackbits(body, j)
                    stats["linelens"][len(line)] = stats["linelens"].get(len(line), 0) + 1
                    rows.append(bytes(line))
                if j != len(body):
                    print(f"block @{i:x}: {len(body) - j} trailing bytes")
            i += 5 + ln
        else:
            i += 1
    width = max((len(r) for r in rows), default=1)
    rows = [r.ljust(width, b"\0") for r in rows]
    if bpp == 2:  # collapse 2-bit pixels to 1 bit: any nonzero drop -> black
        conv = []
        for r in rows:
            bits = []
            for byte in r:
                for s in (6, 4, 2, 0):
                    bits.append(1 if (byte >> s) & 3 else 0)
            packed = bytearray((len(bits) + 7) // 8)
            for k, b in enumerate(bits):
                if b:
                    packed[k // 8] |= 0x80 >> (k % 8)
            conv.append(bytes(packed))
        rows, width = conv, len(conv[0]) if conv else 1
    with open(sys.argv[2], "wb") as f:
        f.write(b"P4\n%d %d\n" % (width * 8, len(rows)))
        for r in rows:
            f.write(r)
    ink_rows = [k for k, r in enumerate(rows) if any(r)]
    print(stats["blocks"], "blocks,", stats["skips"], "skipped blocks,", len(rows), "rows,",
          f"{width * 8}px wide; ink on rows {ink_rows[:1]}..{ink_rows[-1:]} ({len(ink_rows)} rows)")
    print("line lengths (bytes: count):", dict(sorted(stats["linelens"].items())[:8]))


if __name__ == "__main__":
    main()
