#!/usr/bin/env python3
"""Decode a multi-channel Canon iP100 page into per-channel line data.

Usage: colordecode.py FILE.prn [page]   -> prints per-channel stats
As a module: decode(path, page=0) -> dict with 'inks', 'lpb', 'res', 'channels' (list of row lists of bytes)
"""
import sys


def unpackbits(buf, i):
    out = bytearray()
    while i < len(buf):
        c = buf[i]
        if c == 0x80:
            return bytes(out), i + 1
        if c < 0x80:
            out += buf[i + 1:i + 2 + c]
            i += 2 + c
        else:
            out += bytes([buf[i + 1]]) * (257 - c)
            i += 2
    return bytes(out), i


def decode(path, page=0):
    data = open(path, "rb").read()
    i, pg = 0, 0
    inks, lpb, res, tcmd, ccmd = b"K", 16, (600, 600), b"", b""
    chans, band_idx, pre_skip = None, 0, 0
    while i < len(data):
        if data[i] == 0x1B and i + 4 < len(data) and data[i + 1] == 0x28:
            cmd, ln = chr(data[i + 2]), data[i + 3] | data[i + 4] << 8
            body = data[i + 5:i + 5 + ln]
            if pg == page:
                if cmd == "J":
                    lpb = body[0]
                elif cmd == "d":
                    res = (body[0] << 8 | body[1], body[2] << 8 | body[3])
                elif cmd == "t":
                    tcmd = body
                elif cmd == "c":
                    ccmd = body
                elif cmd == "L":
                    inks = body
                    chans = [[b""] * pre_skip for _ in inks]
                    band_idx = 0
                elif cmd == "e":
                    n = int.from_bytes(body, "big")
                    if chans is None:
                        pre_skip += n * lpb
                    else:
                        for ch in chans:
                            ch += [b""] * (n * lpb)
                elif cmd == "F":
                    if chans is None:
                        chans = [[] for _ in inks]
                    ch = band_idx % len(inks)
                    j = 0
                    for _ in range(lpb):
                        line, j = unpackbits(body, j)
                        chans[ch].append(line)
                    band_idx += 1
            i += 5 + ln
        elif data[i] == 0x0C:
            if pg == page and chans is not None:
                break
            pg += 1
            i += 1
        else:
            i += 1
    return {"inks": inks, "lpb": lpb, "res": res, "t": tcmd, "c": ccmd, "channels": chans or []}


def main():
    d = decode(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 0)
    print("inks", d["inks"].hex(), repr(d["inks"]), "lpb", d["lpb"], "res", d["res"], "(t", d["t"].hex(), "(c", d["c"].hex())
    for name, rows in zip(d["inks"], d["channels"]):
        lens = [len(r) for r in rows if r]
        vals = {}
        for r in rows:
            for b in r:
                vals[b] = vals.get(b, 0) + 1
        top = sorted(vals.items(), key=lambda kv: -kv[1])[:8]
        print(f"  {chr(name) if 32 < name < 127 else hex(name)}: rows={len(rows)} nonempty={len(lens)} maxlen={max(lens) if lens else 0} "
              f"top bytes={[(hex(k), v) for k, v in top]}")


if __name__ == "__main__":
    main()
