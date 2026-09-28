# Canon PIXMA iP100 print protocol notes

Worked out by comparing this driver's output with the output of Canon's original Mac
driver (v3.x). Multi-byte values are big-endian unless noted; `ESC (x` commands are
`1B 28 <x> <len16 little-endian> <body>`.

## Job

1. `00`, then `ESC [K` with body `00 1e 00` + `BSSR=DJS,DBS,DWS,DOC,DSC,BST,PID,CHD,OPT,LVR,CIR,CTK,AOF,HRI,MSI;` (status request).
2. Two BJL blocks — `ESC [K` with a 2-byte body `00 1f`, followed by free text:
   `BJLSTART\nControlmode = Common\nSetTime = YYYYMMDDhhmmss\nBJLEND\n` and
   `BJLSTART\nCONTROLMODE=COMMON\nSETSILENT=OFF\nBJLEND\n`.
3. `ESC [K` body `00 0f`.
4. Pages.
5. `ESC (b 00`, then `ESC [K` body `00 1e 00 09` + `SSR=DF;`.

Pages are sent **last page first** (the printer stacks face-up).

## Page

| Command | Body | Meaning |
| --- | --- | --- |
| `(b` | `01` | start (first page only) |
| `(d` | `02 58 02 58` | 600 × 600 dpi (`01 2c 01 2c` = 300 dpi) |
| `(t` | 36 bytes | ink format: `80 80 01` then 3 bytes per ink (see below) |
| `(c` | `30 00 03` | plain paper, standard quality |
| `(p` | 46 bytes | page geometry (below) |
| `(l` | `34 00` | |
| `(u` | `00` | |
| `(s` | `00` / `01` | `01` on the last page of the job |
| `(q` | page number | 1, 2, 3 … in send order |
| `($` | `01 00 00 00 00` | |
| `(b` | `01` | |
| `(I` | `01` | multi-raster mode |
| `(J` | `10` | lines per band (16; 8 at 300 dpi) |
| `(e` | n16 | skip n blank bands |
| `(L` | ink letters | `K` (black only) or `CMYK` — sent just before the first band |
| `(F` | band data | one per ink per band, in `(L` order |
| `(e` | `00 01` | end of page data |
| `0C` | | form feed / eject |

### `(p` geometry (600 dpi dots)

`ceil(area_h/10)16, 0 16, ceil(area_w/10)16, flag16, 0 32, 600 16, left32, top32, area_w32, area_h32, 0 64, paper_w32, paper_h32`.
`flag` is 7 for Letter/Legal (paper wider than the 4800-dot print width), else 0. Canon's
values per size are in `src/rastertoip100.c` (`geometries[]`). Borderless uses 0 for the first
fields and negative left/top (−47).

### Band data (`(F`)

A band is `(J` lines. Each line is PackBits-compressed with trailing zero bytes removed, then
terminated with `80`. A blank line is just `80`. An ink with nothing in the band is sent as a
zero-length `(F`. Bands where every ink is blank are replaced by `(e` skips.

Dot packing per ink (from `(t`: `22 00 03` / `01 00 02`):

- **C, M** (`22 00 03`): 3 levels — 0 = none, 1 = one drop, 2 = two drops — packed **five dots
  per byte in base 3**, first dot in the most significant digit (byte = d0·81 + d1·27 + d2·9 + d3·3 + d4).
- **Y, K** (`01 00 02`): 1 bit per dot, most significant bit first.

## Colour behaviour of Canon's driver (plain paper)

- Pure black (RGB 0,0,0) → pigment black only, at ~95% dot coverage (≈5% evenly scattered gaps).
- Greys → composite C+M+Y up to ~60% grey; black ink joins only for dark greys.
- Pure cyan ≈ 0.86 drops/dot of C plus a little Y; red → M + Y; blue → C (double drops) + M.
- Pages containing no colour are sent black-only (`(L K`).

This driver reproduces that with a 17×17×17 RGB→CMYK table (`src/ip100_plain_lut.h`, built by
`tools/buildlut.py` from the measurements in `data/`) and serpentine error diffusion with a
little threshold noise.

## Not yet implemented (observed)

- 300 dpi fast modes: `(d 01 2c 01 2c`, `(J 08`, all inks 1-bit, `(c 30 00 01`.
- Photo papers: `(c 30 0b 03`, `(L` with nine channels `43 4d 59 6b 83 8d ab c3 cd`, different `(t`.

## Printer status

The USB back-channel returns lines such as
`BST:00;…;CTK:CL,SET,/,BK,SET;CIR:CL=100,BK=100;…` — `CIR` holds ink levels in percent.
