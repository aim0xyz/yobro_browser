#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [ -z "${YOBRO_SIGN_IDENTITY+x}" ] && [ -f .yobro-signing-identity ]; then
  IFS= read -r YOBRO_SIGN_IDENTITY < .yobro-signing-identity
  export YOBRO_SIGN_IDENTITY
fi
# Keep developer names and checkout paths out of shipped Mach-O metadata.
BUILD_ROOT="${YOBRO_BUILD_ROOT:-/private/tmp/yobro-distribution-build-$UID}"
swift build -c release --scratch-path "$BUILD_ROOT" \
  -Xswiftc -file-prefix-map -Xswiftc "$PWD=/YOBRO" \
  -Xswiftc -debug-prefix-map -Xswiftc "$PWD=/YOBRO"
BUILD_BIN="$BUILD_ROOT/release"
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
cp "$BUILD_BIN/YOBRO" "$APP/Contents/MacOS/YOBRO"
cp "$BUILD_BIN/yobro-mcp" "$APP/Contents/MacOS/yobro-mcp"
if [ -d "$BUILD_BIN/Sparkle.framework" ]; then
  mkdir -p "$APP/Contents/Frameworks"
  # Preserve Sparkle's framework symlinks, XPC helpers and executable bits.
  ditto "$BUILD_BIN/Sparkle.framework" "$APP/Contents/Frameworks/Sparkle.framework"
fi
if /usr/bin/otool -L "$APP/Contents/MacOS/YOBRO" | /usr/bin/grep -q '@rpath/Sparkle.framework/'; then
  test -f "$APP/Contents/Frameworks/Sparkle.framework/Versions/B/Sparkle" || {
    echo "Error: Sparkle is linked but missing from the app bundle." >&2; exit 1;
  }
  # SwiftPM links Sparkle through @rpath but does not add the app bundle's
  # Frameworks directory. Without this, dyld aborts before YoBro can start.
  /usr/bin/install_name_tool -add_rpath '@executable_path/../Frameworks' "$APP/Contents/MacOS/YOBRO"
fi
python3 scripts/verify-runtime-linkage.py "$APP"
mkdir -p "$APP/Contents/Resources/YOBRO_YOBRO.bundle" "$APP/Contents/Resources/YOBRO_YOBROMCP.bundle"
# Newer SwiftPM versions lay resource bundles out as Contents/Resources;
# the app keeps the flat layout either way.
RESOURCES="$BUILD_BIN/YOBRO_YOBRO.bundle"
if [ -d "$RESOURCES/Contents/Resources" ]; then RESOURCES="$RESOURCES/Contents/Resources"; fi
# Explicit allowlist. Never copy the checkout, a home directory or runtime state.
for resource in AgentBridge.js MailWorker.py BrowserImport.py DarkReader.js WebAppearance.js \
  DarkReader-LICENSE.txt English.json YOBROMark.png LoginAutofill.js PageSignals.js \
  uBlockOriginLite.safari.zip uBlockOriginLite-NOTICE.txt; do
  cp "$RESOURCES/$resource" "$APP/Contents/Resources/YOBRO_YOBRO.bundle/$resource"
done
cp Sources/YOBROMCP/Resources/tools.json "$APP/Contents/Resources/YOBRO_YOBROMCP.bundle/"
for resource in AgentBridge.js MailWorker.py BrowserImport.py DarkReader.js WebAppearance.js DarkReader-LICENSE.txt; do
  cp "$RESOURCES/$resource" "$APP/Contents/Resources/$resource"
done
cp bin/yobro "$APP/Contents/MacOS/yobroctl"
cp Resources/Info.plist "$APP/Contents/Info.plist"
python3 scripts/verify-resource-bundle.py "$APP"
if [ -n "${YOBRO_UPDATE_FEED_URL:-}" ]; then
  /usr/libexec/PlistBuddy -c "Set :SUFeedURL $YOBRO_UPDATE_FEED_URL" "$APP/Contents/Info.plist"
fi
if [ -f Resources/YOBRO.icns ]; then cp Resources/YOBRO.icns "$APP/Contents/Resources/YOBRO.icns"; fi
python3 scripts/package-agent-integrations.py "$APP" "$BUILD_BIN"
case "$(file -b "$APP/Contents/MacOS/YOBRO")" in
  *Mach-O*) ;;
  *) echo "Error: YOBRO must be a native Mach-O executable." >&2; exit 1 ;;
esac
for executable in "$APP/Contents/MacOS/YOBRO" "$APP/Contents/MacOS/yobro-mcp"; do
  /usr/bin/lipo "$executable" -verify_arch "$(uname -m)"
done
sign_app() {
  local target="$1"
  # Sign embedded executables explicitly; --deep alone can miss resource helpers.
  for helper in "$target/Contents/MacOS/yobro-mcp"; do
    if [ -n "${YOBRO_SIGN_IDENTITY:-}" ]; then
      codesign --force --options runtime --timestamp --sign "$YOBRO_SIGN_IDENTITY" "$helper"
    else
      codesign --force --sign - "$helper"
    fi
  done
  if [ -n "${YOBRO_PROVISIONING_PROFILE:-}" ]; then
    python3 scripts/sign-passkey-build.py "$target"
  else
    rm -f "$target/Contents/embedded.provisionprofile"
    if [ -n "${YOBRO_SIGN_IDENTITY:-}" ]; then
      codesign --force --deep --options runtime --timestamp --entitlements Resources/YoBro.entitlements --sign "$YOBRO_SIGN_IDENTITY" "$target"
    else
      codesign --force --deep --entitlements Resources/YoBro.entitlements --sign - "$target"
    fi
  fi
  codesign --verify --deep --strict "$target"
  python3 scripts/verify-audio-entitlement.py "$target"
}

sign_app "$APP"
python3 scripts/audit-distribution.py "$APP" --report "$STAGING_ROOT/privacy-audit.json"

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
python3 scripts/verify-runtime-linkage.py "$FINAL_APP"
if ! python3 scripts/audit-distribution.py "$FINAL_APP" --report "$STAGING_ROOT/privacy-audit.json"; then
  rm -rf "$FINAL_APP"
  if [ -d "$PREVIOUS_APP" ]; then mv "$PREVIOUS_APP" "$REPLACED_APP"; fi
  exit 1
fi
mv "$STAGING_ROOT/privacy-audit.json" "$OUTPUT_DIR/privacy-audit.json"
echo "Built $FINAL_APP"
