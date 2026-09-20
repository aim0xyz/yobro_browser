# WebKit improvements — 2026-09-17

## Implemented

- Space chat uses Notes tabs as durable memory for saved findings and research. Existing notes are preferred over duplicates; append is the default.
- Note reads are paginated without truncating serialized JSON. Oversized writes fail before modifying the note. A successful note-tool response requires a synchronous session write.
- The agent can list current tabs, open additional research tabs, and close its own tabs. Runs allow 32 rounds instead of eight.
- Sidebar tab targets reject folder drags; drop proposals and execution both revalidate their destinations. Esc clears an aborted drag. Overlay hide no longer steals keyboard focus, respects open folder/extension popups, and cancels stale hide timers on mode changes.
- The auto-hide sidebar now has its own native hosting view with stable drag coordinates. Visibility uses native hiding instead of translating the SwiftUI drop destinations. The nested host does not add another titlebar safe-area inset.
- uBlock Origin Lite's Safari package (2026.914.1325) is bundled with the reviewed YoBro compatibility patch 1 (2026.914.1325.1): recognize WebKit's extension URL scheme as Safari rather than Chromium. Filter lists are unchanged. The reproducible patch script validates the upstream SHA-256; both upstream and bundled digests, license, and source are documented in the resource notice.
- On macOS 15.6+, the blocker installs once per profile and starts through WKWebExtensionController. Disabling or removing it persists across launches. Newer bundled package versions migrate existing installations while preserving extension identity and enabled state. Startup waits for uBOL’s initialized message handler and verifies its Safari engine detection before navigation. The old blocker no longer attaches scripts, rules, or URL rewriting to browser pages.
- Extensions have store search, Safari discovery, and optional Dark Reader/Bitwarden download shortcuts. Installation still displays permissions. Installed extensions retain toggles and load at startup.
- Failed synchronous background startup unloads its extension context. Removing an extension no longer unloads it before its saved configuration can be updated. Closing settings during installation does not delete the package being loaded.

## Validation

- Full Swift suite: 154 tests, zero failures, two skipped.
- Subsequent focused run after note persistence and extension lifecycle changes: 62 tests, zero failures, including paginated escaped note content, oversize append preservation, and the actual bundled Safari package loading/disabling/removal.
- Release build is produced separately so a running app is not replaced.
- Live native mouse checks on disposable tabs confirmed adjacent-tab split creation, moving below the adjacent tab, and moving back above it in the overlay. The fixed sidebar was used as a control. The mouse helper posts intermediate drag events; the single-step UI automation drag was insufficient to exercise AppKit drop tracking reliably.
- Final clean build: split creation onto the immediately preceding and following tabs passed; reordering below the final tab passed with a live WKWebView behind the overlay. Both disposable tabs were closed and the original 54-tab session restored. Focused sidebar/layout suite: 33 tests, zero failures.

## Compatibility and remaining limits

- **YouTube playback is not release-ready.** Live checks on 2026-09-17 with bundled 2026.914.1325.1 still reproduce a stalled player (time 0, decoded frames 0, readyState 0, networkState 3) at `https://www.youtube.com/watch?v=moS5SjJFkt4`. Disabling and re-enabling the blocker can temporarily restore playback, but navigating to another video reproduces it. The startup barrier and engine-detection correction do not fix this remaining issue. Keep uBOL enabled; do not treat an automatic disable/reload as a fix.
- The new synthetic YouTube fixture checks that the real bundled MAIN-world scriptlets remove `adPlacements` while retaining `videoDetails`. This verifies injection, not real YouTube playback or reliable ad blocking. The exact live failure's cause remains unconfirmed. Upstream tracks YouTube problems at https://github.com/uBlockOrigin/uAssets/issues/30158; that alone does not prove this is an upstream bug.

- The included Safari package declares WebKit/Safari 18.6 as its minimum. Older systems have no replacement ad blocker; the extension screen explains the requirement.
- This is uBlock Origin Lite, not the Manifest V2 uBlock Origin engine. Its filtering capability depends on WebKit's declarativeNetRequest support.
- Safari App Store apps cannot be installed wholesale into YoBro. Compatible unpacked WebExtensions, ZIPs, and CRXs can be imported. Native companion APIs and unsupported Chrome APIs remain unavailable.
- Dark Reader/Bitwarden shortcuts do not certify all extension features. Native integration in particular remains limited. Built-in Dark Reader already provides page appearance controls without another extension.
- The bundled blocker is pinned to this reviewed release. New browser bundles can upgrade it; automatic remote upstream package updates are not implemented.
- Automated tests exercise WebKit and native layouts, but do not prove every real website, physical sidebar drag gesture, login flow, or third-party extension works. A broad live-site compatibility audit remains separate work.
- Legacy blocker source/tests remain for comparison; production browser paths no longer reference them. Existing unrelated working-tree edits were preserved.
