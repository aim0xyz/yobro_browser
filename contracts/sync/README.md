# Sync — versioned encrypted schema (engine-neutral)

Source: `Sources/YOBRO/BrowserSync.swift` (`BrowserSyncSnapshot.currentVersion = 1`,
ChaChaPoly payload, `formatVersion`).

- Scope: tabs (without `interactionState`), spaces, folders, bookmarks,
  sanitized history. Explicitly excluded: cookies, passwords, extension
  storage, mail credentials, local file paths.
- Old-reader protection: `snapshot.version <= currentVersion` else reject;
  empty spaces / >5000 tabs / >100000 history / >50000 bookmarks rejected;
  unknown spaces/tabs dropped, never applied blindly.
- Chromium must reuse this envelope or ship a versioned migration with
  old-reader protection (parity contract). No second sync format.
