#!/bin/bash
# Install the iP100 Native driver from a source build. Run with: sudo ./install.sh
# (Most people should use the .pkg installer from the Releases page instead.)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
FILTER_DIR="/Library/Printers/iP100Native"
PPD_DIR="/Library/Printers/PPDs/Contents/Resources"

if [ "$(id -u)" -ne 0 ]; then
  echo "Please run with sudo: sudo \"$0\"" >&2
  exit 1
fi
if [ ! -x "$HERE/build/rastertoip100" ]; then
  echo "build/rastertoip100 not found - run 'make' first." >&2
  exit 1
fi

# CUPS only runs filters that are owned by root and not writable by others.
install -d -o root -g wheel -m 755 "$FILTER_DIR"
install -o root -g wheel -m 755 "$HERE/build/rastertoip100" "$FILTER_DIR/rastertoip100"
install -o root -g wheel -m 644 "$HERE/src/CanoniP100Native.ppd" "$PPD_DIR/CanoniP100Native.ppd"

# Remove the pre-1.0 location, if present.
rm -rf /Library/Printers/Canon/iP100Native

echo "Installed:"
ls -l "$FILTER_DIR/rastertoip100" "$PPD_DIR/CanoniP100Native.ppd"
echo
echo "Now add the printer: System Settings > Printers & Scanners > Add Printer,"
echo "select the iP100 and choose \"Canon iP100 Native (unofficial driver)\" under Use."
