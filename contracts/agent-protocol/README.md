# Agent protocol v2 — versioned contract (frozen in Phase 2)

Source of truth extracted from `Sources/YOBRO/ControlBridge.swift` (`handle(_:)`).
The WebKit implementation and any future Chromium implementation must satisfy
these fixtures. Any shape change needs a protocol version bump.

Transport (both engines):
- Unix socket (`control.sock` / `p-<UUID>.sock`), mode 0600, UID check. No TCP.
- Windows Chromium build: named pipe with owner-only ACL.
- One newline-terminated JSON request → one JSON response: `{"ok":true,"result":{...}}`
  or `{"ok":false,"error":"..."}`. The request payload may contain at most
  exactly 1,048,576 bytes before the terminating LF; a larger payload or a
  missing LF is rejected deterministically as invalid framing.
- Single-command serialization applies after dispatch precedence. `status`
  always bypasses busy state; enabled `tabs` and enabled `end` also bypass it.
  Every other concurrent command is rejected with
  `Agent is busy. Retry after the current command completes.`
- Inactive profile: `This profile is inactive...`; paused: `Agentenzugriff ist
  im Browser pausiert.` / `Agent operation was paused.`
- User tabs rejected: `This is a user tab. Open its URL with new to work in the right agent pane.`
- Blocked (user workspace): `space`, `panel`, `restore`, `move` →
  `This command changes the user workspace and is unavailable in agent mode.`
- `history` / `downloads` require opt-in, else the "Verlauf und Downloads sind
  für die Agentenschnittstelle gesperrt..." message.
- `open`/`reload`/`back`/`forward`, `new` when a URL key is present,
  `read`, and `find` wait up to 20s; still-loading →
  `Seite lädt noch. Später erneut mit read prüfen.`
- `end` discards all agent-owned tabs; it never converts a bridge-injected
  agent page into a user-owned page. `close` preserves the current WebKit
  response quirk: its returned snapshot is produced after ownership removal.
- `read` returns `{tab, page}` with a document UUID; `click`/`fill` need
  `ref` + `document` from a current read, else `ref und document aus einem
  aktuellen read sind erforderlich.` Fill without value → `Wert fehlt.`
- Click/fill responses carry `next: "Read again to verify the outcome; pages may update asynchronously."`
- Downloads only http(s) with host; response: `{"started":true,"next":"Use downloads to check progress..."}`.

AgentBridge boundary (both engines):
- The shared bridge runs only in an isolated content/application world on
  agent-owned top-level pages. User-owned pages and subframes receive no bridge.
- Interactive elements inside nested **open** shadow roots are included and can
  be acted on. Closed shadow roots remain inaccessible.
- Iframes expose only top-level metadata (`src`, `title`, and
  `note: "Frame content is not included."`). Their document content and refs
  are excluded, including same-origin frames.
- Password values are always `[redacted]`; `fill` on file inputs is rejected.
- Full-document navigation and BFCache restoration rotate the document UUID.
  Detached/reinserted elements receive an actionable ref again only after a
  fresh snapshot.

`fixtures/bridge-boundaries.json` freezes these boundary rules without freezing
random document UUIDs or generated element refs.
