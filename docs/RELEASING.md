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

## Keep uBlock Origin Lite current

YoBro ships uBlock Origin Lite inside the app. uBOL compiles its filter lists
into each release, the bundled copy never updates itself, and YouTube breaks
older releases within weeks. Users only get a newer one with a YoBro release.

`scripts/publish-release.sh` checks the bundled release against
[uBOL-home](https://github.com/uBlockOrigin/uBOL-home/releases) and against
the minimum named in
[uAssets#30158](https://github.com/uBlockOrigin/uAssets/issues/30158). It
stops below that minimum. It also stops when the bundle is behind the latest
release or the check cannot reach GitHub; set `YOBRO_ALLOW_STALE_UBLOCK=1` to
publish anyway. `scripts/build.sh` warns, offline, when the bundled release is
more than 14 days old.

To update the bundled release:

1. Run `python3 scripts/check-ublock-freshness.py`. When upstream is newer, it
   prints the release page and the update command with the Safari asset's
   SHA-256.
2. Read the release notes on that page.
3. Run the printed command:
   `python3 scripts/prepare-bundled-ublock.py --tag <tag> --sha256 <digest>`.
   It downloads the official Safari zip, verifies the digest, applies the
   YoBro WebKit patch, and rewrites `uBlockOriginLite.safari.zip` and its
   notice in `Sources/YOBRO/Resources`.
   - If it reports that engine detection changed in `js/ext.js`, review the
     patch by hand before going further.
   - If it reports changed permissions, review them first. Existing installs
     keep the permissions of the previous version, so a new permission stays
     ungranted. Rerun with `--accept-permission-changes` only once that is
     handled.
4. Run `swift test` and `python3 -m unittest tests/test_ublock_bundle.py`.
   `testBundledBlockerYouTubeStallRecoveryTerminates` replays YouTube's
   ad-block stall offline.
5. Play a few YouTube videos in a throwaway profile, never your own:
   `YOBRO_HOME="$(mktemp -d)" .build/debug/YOBRO`.
6. Update the uBOL entry in `docs/WEBKIT-IMPROVEMENTS.md` and commit.

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
