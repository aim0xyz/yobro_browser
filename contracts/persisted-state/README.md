# Persisted state — versioned schemas + migration fixtures

Extracted from `BrowserModel.swift` (`StoredTab`, `StoredSession`, `BridgePolicy`),
`BrowserOrganization.swift` (`TabFolder`, `StoredSplit`), `BrowserImport.swift`
(`BookmarkEntry`), `LibraryModel.swift` (`HistoryEntry`, `DownloadEntry`),
`BrowserProfiles.swift` (`Registry`, `LocalBrowserProfile`), `BrowserSync.swift`.

Homes:
- WebKit (unchanged): `~/Library/Application Support/YOBRO` (`YOBRO_HOME` override in tests).
  Original profile lives at root; others at `Profiles/<UUID>/`.
- Any future Chromium Preview must use a separate per-OS home. It may read
  WebKit state **only** via a migration importer, never by opening live files.

Files per profile home:
`session.json` (StoredSession), `history.json` ([HistoryEntry]),
`bookmarks.json` ([BookmarkEntry]), `downloads.json` ([DownloadEntry]),
`bridge-policy.json` (BridgePolicy), `download-preferences.json`,
`space-chat.json`, `chat-connection.json`, mail/proxy/vpn/appearance/zoom stores.
Root: `profiles.json` (Registry), `control.sock` / `p-<UUID>.sock` (sockets, not data).

Rules (migration plan §5 + Phase 7 gate):
- Any format change needs a versioned, reversible migration + fixtures here.
- Corrupt files are quarantined to `<name>.corrupt[-N]`, never silently
  overwritten (`PersistedState.swift`).
- Atomic writes everywhere; migration journal + reset/reimport in the new app.
- Source profile left untouched; preview counts + exclusions shown before import.
- Secrets (Keychain, mail credentials, cookies, extension storage) never enter
  logs, sync payloads, crash reports, or temp files.
