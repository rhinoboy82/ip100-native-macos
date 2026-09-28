# iP100 Native — unofficial macOS driver for the Canon PIXMA iP100

A small, native printer driver that lets the **Canon PIXMA iP100** portable inkjet
print from current macOS on **Apple Silicon and Intel** Macs, without Rosetta and
without Canon's original (Intel-only, no longer maintained) driver.

> **Unofficial, unsupported, no warranty.** This project is not made, endorsed or
> supported by Canon. It is provided as is, with **no support of any kind** and
> **no liability** for any damage or loss. See [Disclaimer](#disclaimer).

## What works

- Plain paper, **colour** and **black & white**
- **Print Quality** on plain paper: Super Fine, Standard (600 dpi), Draft and Fast (300 dpi)
- **Photo papers**: Photo Paper Pro, Photo Paper Plus Glossy II, Photo Paper Plus Glossy,
  Photo Paper Plus Semi-gloss, Glossy Photo Paper, Matte Photo Paper, High Resolution Paper;
  and **Envelope** mode
- **Borderless** printing on photo papers: Letter, A4, 4×6, 5×7, 8×10
- Paper sizes: Letter, Legal, A4, A5, B5, 4×6, 5×7, 8×10, #10 envelope, and **custom sizes**
  from 54 × 86 mm up to 8.5 × 14 in
- USB connection
- Pages print last-page-first so the face-up stack comes out in order (as Canon's driver does)
- Pages with no colour automatically print with black ink only
- **Ink Density** option (Full / Normal / Light / Draft), useful on thin or cheap paper
- **Ink levels** in System Settings (Printers & Scanners → the printer → Options & Supplies → Supply Levels),
  updated after each job

Not supported: T-shirt transfers, nozzle check and cleaning from the Mac (use the printer's
button, see Troubleshooting), Bluetooth.

## Install

1. **If Canon's original iP100 driver is installed, remove it first** — see
   [Removing Canon's old driver](#removing-canons-old-driver). Otherwise macOS may keep
   choosing it, or show its settings instead of this driver's.
2. Download the latest **`iP100Native-x.y.z.pkg`** from the [Releases](../../releases) page and open it.
3. Connect the iP100 by USB and turn it on.
4. **System Settings → Printers & Scanners → Add Printer, Scanner, or Fax**, select the iP100,
   and under **Use** choose **Select Software…** → **Canon iP100 Native (unofficial driver)**.
   If an iP100 printer was already set up (with Canon's driver or an earlier version of this one),
   **remove it and add it again** this way.

**Updating:** installing a newer `.pkg` updates printers already set up with this driver. If the
print dialog then shows old options, remove the printer and add it again (see Troubleshooting).

Print options are in the print dialog under **Printer Options → Printer Features**
(on some apps click **Show Details** first). The first time you open it for a newly added
printer it can take a few seconds to appear — wait rather than clicking again.

| Option | Choices |
| --- | --- |
| Color Mode | Color (default), Black and White |
| Paper Type | **Plain Paper** (default), photo papers (see above), Envelope |
| Print Quality | Super Fine, **Standard** (default), Draft (300 dpi), Fast (300 dpi, quickest) — Super Fine, Draft and Fast are for plain paper |
| Media Size | Standard sizes, custom sizes, and *(Borderless)* sizes for photo papers |
| Ink Density | Full 100%, **Normal 95%** (default, matches Canon), Light 85%, Draft 75% |

### Uninstall

```bash
sudo ./uninstall.sh
```

(from a copy of this repository). This removes the driver files and any printers set up with it.

## Troubleshooting

- **Power light keeps flashing after a print, printer stops responding** — switch the printer
  off and on. Versions before 1.2.0 could leave the printer in this state after long jobs
  (photos, Super Fine); update to 1.2.0 or later.
- **Print dialog shows the wrong options, no options, or an old model name** — macOS keeps its
  own cached picture of each printer. Remove the printer in System Settings → Printers &
  Scanners and add it again, choosing **Canon iP100 Native (unofficial driver)**. If you see
  Canon options such as "ProfileID" or "Halftoning", Canon's old driver is still installed —
  see below.
- **"CIJAutoSetupTool" warning at login** — a leftover Canon helper that automatically
  re-configures Canon printers when connected (and can undo your setup). Remove Canon's old
  driver (below) and turn it off in System Settings → General → Login Items & Extensions.
- **Supply Levels says "Information not available"** — levels are read at the end of each print
  job; print a page and look again.
- **Colours banded, streaky or missing** — the print head nozzles are partly clogged, which is
  common when the printer hasn't been used for a while or after fitting a new cartridge.
  Load plain paper, hold **RESUME/CANCEL** until the power light flashes **twice** to print a
  nozzle check, and hold it until it flashes **once** to run a cleaning cycle. Repeat the
  cleaning until the nozzle check pattern has no gaps.
- **Text looks fuzzy on thin paper** — choose **Ink Density: Light**.
- **Non-Canon photo paper** — choose the closest Paper Type; for ordinary glossy inkjet photo
  paper, **Photo Paper Plus Glossy** works well.
- **iPhone HDR photos print too dark or washed out** — export the photo as a standard JPEG
  (in Photos: File → Export, JPEG) and print that.

### Removing Canon's old driver

Canon's iP100 driver is Intel-only and does not work on current macOS, but if it is still
installed macOS may pick it (or its settings) instead of this driver. Remove it with:

```bash
sudo rm -rf /Library/Printers/Canon/BJPrinter
```

```bash
sudo rm -f /Library/Printers/PPDs/Contents/Resources/CanonIJiP100series.ppd.gz
```

This leaves Canon's scanner software (`/Library/Printers/Canon/IJScanner`) in place. Then turn
off any **CIJAutoSetupTool** / Canon IJ entry in System Settings → General → Login Items &
Extensions, restart, and remove and re-add the printer as described in Install.
- **Print system logs** — `/var/log/cups/error_log`; the driver logs lines starting with `rastertoip100`.

## How it works

macOS renders each page to a bitmap (CUPS raster). The driver (`src/rastertoip100.c`) converts
that into the iP100's own command language: page setup, colour conversion to C/M/Y/K ink
amounts, error-diffusion halftoning, and Canon's compressed band format.

The command sequence, page geometry and colour behaviour were worked out by comparing the
output of Canon's original driver with this one, byte for byte and ink dot for ink dot. The
colour table (`src/ip100_plain_lut.h`) is generated by `tools/buildlut.py` from ink-coverage
measurements in `data/`. Protocol notes are in [docs/PROTOCOL.md](docs/PROTOCOL.md).
No Canon code or files are included.

## Building from source

Requires the Xcode Command Line Tools.

```bash
make                 # universal (arm64 + x86_64) build/rastertoip100
sudo ./install.sh    # install the local build
```

Signed installer (maintainers): `make pkg` runs `packaging/build-pkg.sh`, which signs with a
Developer ID, builds the `.pkg` and notarizes it. One-time notarization setup:

```bash
xcrun notarytool store-credentials ip100-notary --apple-id <apple-id> --team-id <team-id>
```

## Disclaimer

This is an unofficial, independently developed driver. It is **not** made, endorsed, supported
or approved by Canon Inc. "Canon", "PIXMA" and "iP100" are trademarks of Canon Inc., used only
to identify the compatible printer.

**No support:** 1010ths Development Corporation and the contributors provide no technical
support, maintenance, updates or assistance, and are under no obligation to respond to
questions, issues or pull requests.

**No liability:** you use this software entirely at your own risk. To the maximum extent
permitted by law, 1010ths Development Corporation and the contributors are not liable for any
damages, including damage to printers, print heads or cartridges, wasted ink or paper, or loss
of data or use. Full terms are in [LICENSE](LICENSE).

## License

[MIT](LICENSE) © 2026 1010ths Development Corporation, with the additional disclaimer above.
