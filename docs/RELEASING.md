# Releasing YoBro

YoBro is released from the Mac that owns the non-exportable `Developer ID
Application` signing identity. This keeps the Apple signing key out of GitHub
Actions and out of the repository.

## One-time notary setup

Create an app-specific Apple ID password, then store it locally with:

```bash
xcrun notarytool store-credentials "yobro-notary" \
  --apple-id "YOUR_APPLE_ID" \
  --team-id "UW6QNH59X9" \
  --password "APP_SPECIFIC_PASSWORD"
```

The password is stored in the macOS Keychain, not in this repository.

## Publish

The Sparkle Ed25519 key must be available in the local Keychain. The release
script reads it only to sign the appcast; it never puts it in the release files.

```bash
export YOBRO_NOTARY_PROFILE=yobro-notary
scripts/publish-release.sh v0.6.8
```

The command produces a Developer-ID-signed and notarized DMG, signs
`appcast.xml`, and publishes the DMG, SHA-256 checksum and update feed as a
public GitHub Release.
