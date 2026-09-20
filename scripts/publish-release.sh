#!/bin/bash
# Build, notarize, sign and publish a public YoBro release from the Mac that
# owns the non-exportable Developer ID identity. The signing key never leaves
# this machine; GitHub only hosts the finished, verified artifacts.
set -euo pipefail
cd "$(dirname "$0")/.."

TAG="${1:-}"
if [[ -z "$TAG" ]]; then
  echo "Usage: YOBRO_NOTARY_PROFILE=<keychain-profile> scripts/publish-release.sh vX.Y.Z" >&2
  exit 2
fi
if [[ "$TAG" != v* ]]; then
  echo "Release tags must start with v, for example v0.6.8." >&2
  exit 2
fi
if [[ -z "${YOBRO_SIGN_IDENTITY:-}" && -f .yobro-signing-identity ]]; then
  IFS= read -r YOBRO_SIGN_IDENTITY < .yobro-signing-identity
  export YOBRO_SIGN_IDENTITY
fi
if [[ "${YOBRO_SIGN_IDENTITY:-}" != "Developer ID Application:"* ]]; then
  echo "YOBRO_SIGN_IDENTITY must name a Developer ID Application identity." >&2
  exit 1
fi
if [[ -z "${YOBRO_NOTARY_PROFILE:-}" ]]; then
  echo "Set YOBRO_NOTARY_PROFILE to the name of a notarytool Keychain profile." >&2
  exit 1
fi
if gh release view "$TAG" --repo aim0xyz/yobro_browser >/dev/null 2>&1; then
  echo "Release $TAG already exists; refusing to overwrite it." >&2
  exit 1
fi

scripts/build-dmg.sh --release
VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' dist/YoBro.app/Contents/Info.plist)
if [[ "$TAG" != "v$VERSION" ]]; then
  echo "Tag $TAG does not match app version $VERSION." >&2
  exit 1
fi
DMG=$(find dist -maxdepth 1 -type f -name "YoBro-$VERSION-*.dmg" -print -quit)
if [[ -z "$DMG" ]]; then
  echo "Release DMG was not created." >&2
  exit 1
fi
.build/artifacts/sparkle/Sparkle/bin/generate_appcast \
  --download-url-prefix "https://github.com/aim0xyz/yobro_browser/releases/download/$TAG" \
  --link "https://github.com/aim0xyz/yobro_browser" \
  -o dist/appcast.xml dist
gh release create "$TAG" "$DMG" "$DMG.sha256" dist/appcast.xml \
  --repo aim0xyz/yobro_browser \
  --title "YoBro $VERSION" \
  --generate-notes
echo "Published YoBro $VERSION: https://github.com/aim0xyz/yobro_browser/releases/tag/$TAG"
