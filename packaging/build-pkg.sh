#!/bin/bash
# Build a signed, notarized installer: dist/iP100Native-<version>.pkg
#
# Requirements:
#   - Developer ID Application + Developer ID Installer certificates in the login keychain
#   - A notarytool keychain profile (one-time setup, see README "Building"):
#       xcrun notarytool store-credentials ip100-notary --apple-id <id> --team-id <team>
# Environment overrides: VERSION, APP_IDENTITY, INSTALLER_IDENTITY, NOTARY_PROFILE, SKIP_NOTARIZE=1
set -euo pipefail

cd "$(dirname "$0")/.."
VERSION="${VERSION:-1.0.0}"
TEAM="7636LY3529"
APP_IDENTITY="${APP_IDENTITY:-Developer ID Application: 1010ths Development Corporation ($TEAM)}"
INSTALLER_IDENTITY="${INSTALLER_IDENTITY:-Developer ID Installer: 1010ths Development Corporation ($TEAM)}"
NOTARY_PROFILE="${NOTARY_PROFILE:-ip100-notary}"
PKG_ID="com.1010ths.pkg.ip100native"

STAGE="build/pkgroot"
DIST="dist"
OUT="$DIST/iP100Native-$VERSION.pkg"

rm -rf "$STAGE" build/component.pkg "$DIST"
mkdir -p "$STAGE/Library/Printers/iP100Native" "$STAGE/Library/Printers/PPDs/Contents/Resources" "$DIST"

# 1. Sign the filter (hardened runtime, secure timestamp).
cp build/rastertoip100 "$STAGE/Library/Printers/iP100Native/rastertoip100"
chmod 755 "$STAGE/Library/Printers/iP100Native/rastertoip100"
codesign --force --options runtime --timestamp \
  --identifier com.1010ths.ip100native.rastertoip100 \
  --sign "$APP_IDENTITY" "$STAGE/Library/Printers/iP100Native/rastertoip100"
codesign --verify --strict --verbose=2 "$STAGE/Library/Printers/iP100Native/rastertoip100"

cp src/CanoniP100Native.ppd "$STAGE/Library/Printers/PPDs/Contents/Resources/CanoniP100Native.ppd"
chmod 644 "$STAGE/Library/Printers/PPDs/Contents/Resources/CanoniP100Native.ppd"

# 2. Component package (files owned by root:wheel, as CUPS requires for filters).
#    Strip extended attributes so no AppleDouble (._*) files end up in the payload.
xattr -cr "$STAGE"
export COPYFILE_DISABLE=1
pkgbuild --root "$STAGE" --identifier "$PKG_ID" --version "$VERSION" \
  --ownership recommended --install-location / \
  --scripts packaging/scripts build/component.pkg

# 3. Product archive with licence/disclaimer screens, signed with Developer ID Installer.
python3 packaging/unwrap.py LICENSE packaging/resources/LICENSE.txt
sed "s/@VERSION@/$VERSION/g" packaging/distribution.xml > build/distribution.xml
productbuild --distribution build/distribution.xml --resources packaging/resources \
  --package-path build --sign "$INSTALLER_IDENTITY" --timestamp "$OUT"
rm -f packaging/resources/LICENSE.txt
pkgutil --check-signature "$OUT"

# 4. Notarize and staple.
if [ "${SKIP_NOTARIZE:-0}" = "1" ]; then
  echo "Skipping notarization (SKIP_NOTARIZE=1)."
elif xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null 2>&1; then
  xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple "$OUT"
  spctl --assess --type install --verbose=2 "$OUT"
else
  echo "Notary profile '$NOTARY_PROFILE' not found; package is signed but NOT notarized." >&2
  echo "Set it up once with: xcrun notarytool store-credentials $NOTARY_PROFILE --apple-id <apple-id> --team-id $TEAM" >&2
  exit 2
fi

shasum -a 256 "$OUT"
echo "Built $OUT"
