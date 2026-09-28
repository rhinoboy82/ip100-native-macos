#!/bin/bash
# Remove the iP100 Native driver. Run with: sudo ./uninstall.sh
# Printers you added with this driver are removed too.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  echo "Please run with sudo: sudo \"$0\"" >&2
  exit 1
fi

PPD="/Library/Printers/PPDs/Contents/Resources/CanoniP100Native.ppd"

# Remove any print queues that use this driver.
for ppd in /etc/cups/ppd/*.ppd; do
  [ -e "$ppd" ] || continue
  if grep -q 'iP100Native/rastertoip100' "$ppd"; then
    queue="$(basename "$ppd" .ppd)"
    echo "Removing printer $queue"
    lpadmin -x "$queue" || true
  fi
done

rm -rf /Library/Printers/iP100Native /Library/Printers/Canon/iP100Native
rm -f "$PPD"
pkgutil --forget com.1010ths.pkg.ip100native >/dev/null 2>&1 || true

echo "iP100 Native driver removed."
