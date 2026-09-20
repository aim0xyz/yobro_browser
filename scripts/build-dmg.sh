#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
MODE="${1:---local}"
if [[ "$MODE" != "--local" && "$MODE" != "--release" ]]; then
  echo "Usage: scripts/build-dmg.sh [--local | --release]" >&2; exit 2
fi
if [[ -z "${YOBRO_SIGN_IDENTITY+x}" && -f .yobro-signing-identity ]]; then
  IFS= read -r YOBRO_SIGN_IDENTITY < .yobro-signing-identity
  export YOBRO_SIGN_IDENTITY
fi
if [[ "$MODE" == "--release" ]]; then
  if [[ "${YOBRO_SIGN_IDENTITY:-}" != "Developer ID Application:"* || -z "${YOBRO_NOTARY_PROFILE:-}" ]]; then
    echo "Public release requires a Developer ID Application identity and YOBRO_NOTARY_PROFILE (stored Keychain profile name)." >&2
    exit 1
  fi
fi
./scripts/build.sh
OUTPUT_DIR="${YOBRO_OUTPUT_DIR:-$PWD/dist}"
APP="$OUTPUT_DIR/YoBro.app"
VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist")
ARCH=$(uname -m)
STAGING=$(mktemp -d "$OUTPUT_DIR/.dmg-stage.XXXXXX")
trap 'rm -rf "$STAGING"' EXIT
mkdir "$STAGING/volume"
if [[ "$MODE" == "--release" ]]; then
  # Staple the app as well as the DMG so the copied app can be checked offline.
  ditto -c -k --keepParent "$APP" "$STAGING/YoBro-notarize.zip"
  xcrun notarytool submit "$STAGING/YoBro-notarize.zip" --keychain-profile "$YOBRO_NOTARY_PROFILE" --wait
  xcrun stapler staple "$APP"
  xcrun stapler validate "$APP"
  spctl --assess --type execute "$APP"
  python3 scripts/audit-distribution.py "$APP" --report "$OUTPUT_DIR/privacy-audit.json"
fi
ditto "$APP" "$STAGING/volume/YoBro.app"
ln -s /Applications "$STAGING/volume/Applications"
cp packaging/INSTALL.txt "$STAGING/volume/INSTALL.txt"
SUFFIX="-local"
if [[ "$MODE" == "--release" ]]; then SUFFIX=""; fi
NAME="YoBro-$VERSION-$ARCH$SUFFIX.dmg"
hdiutil create -volname "YoBro" -srcfolder "$STAGING/volume" -format UDZO -ov "$STAGING/$NAME"
if [[ "$MODE" == "--release" ]]; then
  codesign --force --timestamp --sign "$YOBRO_SIGN_IDENTITY" "$STAGING/$NAME"
  xcrun notarytool submit "$STAGING/$NAME" --keychain-profile "$YOBRO_NOTARY_PROFILE" --wait
  xcrun stapler staple "$STAGING/$NAME"
  xcrun stapler validate "$STAGING/$NAME"
  spctl --assess --type open --context context:primary-signature "$STAGING/$NAME"
fi
hdiutil verify "$STAGING/$NAME"
mv "$STAGING/$NAME" "$OUTPUT_DIR/$NAME"
(cd "$OUTPUT_DIR" && shasum -a 256 "$NAME" > "$NAME.sha256")
echo "Built $OUTPUT_DIR/$NAME"
if [[ "$MODE" == "--local" ]]; then echo "Local test build only: not notarized for public distribution."; fi
