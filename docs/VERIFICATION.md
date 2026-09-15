# Verification — 5 September 2026

## YOBRO Mail 0.3

Six native Swift tests and nine Python transport/MIME tests passed. The Python tests also passed with the `/usr/bin/python3` 3.9 runtime used by the app. A real Swift-to-Python helper invocation verified safe failure handling against a closed local port. All original browser integration tests passed after the mail workspace was added. Release packaging, embedded helper presence, local signature and bundle startup were verified; the internal Mail workspace opened through the bundled CLI. The dark Mail welcome view was rendered and visually inspected (`docs/mail-preview.png`).

No real mailbox credentials were supplied and no real mail was sent. Provider interoperability, actual SMTP delivery, Keychain prompts for real accounts, and populated mailbox UI still require user-account testing. OAuth-only providers remain unsupported. Full mail scope and limitations are recorded in `docs/MAIL.md`.

## System appearance

Adaptive AppKit colors replace forced light mode across the browser shell and library. Debug compilation passed and a separate dark-appearance app window was rendered and visually inspected (`docs/dark-preview.png`), without modifying the user's global macOS settings. The test override is enabled only alongside `YOBRO_DEV_CAPTURE=1`; normal launches inherit system appearance. Cross-platform sync was deferred at the user's request.

## YOBRO 0.2 follow-up

- `tests/daily_browser.py` passed against the native app: in-page search (including no match and backward search), filtered history and repeat visits, tab duplication, ordering and reopening.
- Native WebKit downloads tested with cookie-protected fixture URLs, via both page links and the agent command. Saved files match the expected bytes exactly.
- Two downloads with the same suggested name produce separate files; the first file remains unchanged.
- Cancellation removes partial files. A deliberately truncated HTTP response produces a failed download without a final file.
- History plus completed, cancelled and failed download records verified after restarting the app.
- The original full agent integration suite also passed after the 0.2 changes.
- Quick switcher and download library rendered and visually inspected through YOBRO's own development capture endpoint.

The drag gesture itself and Finder reveal are not automated by these tests; the underlying tab-order operation is tested through the same model used by drag/drop. Download resume is not implemented.

Tested on the user's Apple Silicon Mac, macOS 26.5.1, Swift 6.3.3.

## Passed

- Debug and optimized release compilation, with no remaining compiler warnings.
- Native Mach-O executable and ad-hoc app signature verification.
- Full integration suite against both the development executable and the packaged `YoBro.app`.
- Real localhost page navigation and DOM snapshots in WebKit's isolated content world.
- Page-defined `window.__yobro` could not override the isolated bridge.
- Password field values redacted from snapshots.
- Filling text containing Unicode, quotes and backslashes.
- Native HTML form submission and reading the resulting page.
- Checkbox interaction and open shadow-root button interaction.
- Stable element references in one document; old references rejected after navigation.
- Disabled controls, unsupported schemes, unknown tabs and arbitrary evaluation rejected.
- Cookie shared across two tabs, focus, pinning, split toggling and space switching.
- Session data written with correct selected tab.
- Bundled CLI connected successfully to the packaged app.
- Public GitHub repository loaded and read through YOBRO's own interface: 129 interactive elements, 39 headings, 13,520 text characters at the time of testing.
- App window captured through native rendering and visually inspected; corrected initial headline truncation and locale.

No Chromium, Playwright, browser extension, or computer-use automation was used to control these pages. Window capture was only used to inspect the native shell's appearance, not for agent navigation.

## Not established by these tests

Compatibility with all websites, real account sign-in flows, downloads, media permissions, cross-origin frame interaction, extension support, or long-running daily-browser stability. Tests verify session-file persistence, not restoration of full navigation history or form state. This is a native browser prototype, not complete Arc feature parity.
