#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [ -z "${YOBRO_SIGN_IDENTITY+x}" ] && [ -f .yobro-signing-identity ]; then
  IFS= read -r YOBRO_SIGN_IDENTITY < .yobro-signing-identity
  export YOBRO_SIGN_IDENTITY
fi
swift build -c release
OUTPUT_DIR="${YOBRO_OUTPUT_DIR:-$PWD/dist}"
FINAL_APP="$OUTPUT_DIR/YoBro.app"
LEGACY_APP="$OUTPUT_DIR/YOBRO.app"
mkdir -p "$OUTPUT_DIR"
STAGING_ROOT="$(mktemp -d "$OUTPUT_DIR/.yobro-build.XXXXXX")"
APP="$STAGING_ROOT/YoBro.app"
PREVIOUS_APP="$OUTPUT_DIR/.YOBRO.previous.$$"
REPLACED_APP=""
cleanup() {
  rm -rf "$STAGING_ROOT"
  if [ -d "$PREVIOUS_APP" ] && [ -d "$FINAL_APP" ]; then rm -rf "$PREVIOUS_APP"; fi
}
trap cleanup EXIT
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp .build/release/YOBRO "$APP/Contents/MacOS/YOBRO"
cp -R .build/release/YOBRO_YOBRO.bundle "$APP/Contents/Resources/"
cp .build/release/YOBRO_YOBRO.bundle/AgentBridge.js "$APP/Contents/Resources/AgentBridge.js"
cp .build/release/YOBRO_YOBRO.bundle/MailWorker.py "$APP/Contents/Resources/MailWorker.py"
for resource in BrowserImport.py DarkReader.js WebAppearance.js DarkReader-LICENSE.txt; do
  cp ".build/release/YOBRO_YOBRO.bundle/$resource" "$APP/Contents/Resources/$resource"
done
cp bin/yobro "$APP/Contents/MacOS/yobroctl"
cp Resources/Info.plist "$APP/Contents/Info.plist"
if [ -f Resources/YOBRO.icns ]; then cp Resources/YOBRO.icns "$APP/Contents/Resources/YOBRO.icns"; fi
case "$(file -b "$APP/Contents/MacOS/YOBRO")" in
  *Mach-O*) ;;
  *) echo "Error: YOBRO must be a native Mach-O executable." >&2; exit 1 ;;
esac
sign_app() {
  local target="$1"
  if [ -n "${YOBRO_PROVISIONING_PROFILE:-}" ]; then
    python3 scripts/sign-passkey-build.py "$target"
  else
    rm -f "$target/Contents/embedded.provisionprofile"
    if [ -n "${YOBRO_SIGN_IDENTITY:-}" ]; then
      codesign --force --deep --options runtime --timestamp --sign "$YOBRO_SIGN_IDENTITY" "$target"
    else
      codesign --force --deep --sign - "$target"
    fi
  fi
  codesign --verify --deep --strict "$target"
}

sign_app "$APP"

# Never overwrite a running app's executable in place. macOS validates signed
# pages lazily and will SIGKILL the old process if its on-disk binary changes.
if [ -d "$FINAL_APP" ]; then
  mv "$FINAL_APP" "$PREVIOUS_APP"
  REPLACED_APP="$FINAL_APP"
elif [ -d "$LEGACY_APP" ]; then
  mv "$LEGACY_APP" "$PREVIOUS_APP"
  REPLACED_APP="$LEGACY_APP"
fi
if ! mv "$APP" "$FINAL_APP"; then
  if [ -d "$PREVIOUS_APP" ]; then mv "$PREVIOUS_APP" "$REPLACED_APP"; fi
  exit 1
fi
# Re-sign at the final path. On some macOS setups the bundle move can update
# executable metadata after the staging signature was sealed.
if ! sign_app "$FINAL_APP"; then
  rm -rf "$FINAL_APP"
  if [ -d "$PREVIOUS_APP" ]; then mv "$PREVIOUS_APP" "$REPLACED_APP"; fi
  exit 1
fi
echo "Built $FINAL_APP"
