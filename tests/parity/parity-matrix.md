# Parity matrix — WebKit reference vs planned Chromium implementation

States: `not started` | `implemented` | `verified-1` (one platform) | `verified-all`.

The discarded Phase-1 prototype has been removed. Chromium entries below track a future implementation from a clean baseline; prototype-only behavior does not count as implemented.

## Shell / navigation

| Capability | WebKit | Chromium | Notes |
|---|---|---|---|
| Light/dark adaptive theme, exact tokens, spacing, typography, colors, hover and focus | verified-all | not started | Golden references must be recaptured and reviewed |
| Expanded and compact sidebars | verified-all | not started | |
| Tabs, pins, Spaces, colored folders, drag reorder, closed-tab restore, duplicate and split view | verified-all | not started | |
| Start page, address/search, quick switcher, find, zoom, favicons, shortcuts and window controls | verified-all | not started | |
| Popups, permissions, certificates, downloads, media state and crash recovery | verified-all | not started | |

## Profiles / data

| Capability | WebKit | Chromium | Notes |
|---|---|---|---|
| One persistent context per YOBRO profile; separate cookies and storage | verified-all | not started | Separate application identity and data directory required |
| Tabs, Spaces, folders, notes, bookmarks, history, downloads, settings and session restore | verified-all | not started | |
| Import Safari/Chrome/Brave/Arc/Firefox/WebKit-YOBRO with preview and selection | verified-all | not started | Read-only importer, journal, reset and reimport required |
| Passwords via platform credential adapter | verified-all | not started | Exact HTTPS origin and confirm-to-save |
| Cookie/session migration explicit and previewed, never silent | verified-all | not started | Auth-site test set remains in `docs/CHROMIUM-SCOPE.md` |
| Encrypted sync payload compatible or versioned with old-reader protection | verified-all | not started | `contracts/sync` |

## Extensions

| Capability | WebKit | Chromium | Notes |
|---|---|---|---|
| Unpacked/ZIP/CRX install, permission approval, lifecycle, workers, content scripts, toolbar and popups | verified-all | not started | Requires an engine/API feasibility gate before UI work |
| Per-profile install and storage | verified-all | not started | |
| Web Store acquisition where legally and technically viable | verified-all | not started | Scope: `docs/CHROMIUM-SCOPE.md` |
| Compatibility matrix by API and representative extension set | verified-all | not started | Ten-extension test set remains defined in the scope document |

## Agent

| Capability | WebKit | Chromium | Notes |
|---|---|---|---|
| Protocol v2 response shapes and commands | verified-all | not started | `contracts/agent-protocol` remains frozen |
| User-versus-agent tabs, right pane, pause, invalidation, serialization, activity and library opt-in | verified-all | not started | |
| Isolated bridge, document IDs, stale rejection, frames, secret redaction and post-action reads | verified-all | not started | No public CDP or remote-debugging port |

## Mail / assistant / sync

| Capability | WebKit | Chromium | Notes |
|---|---|---|---|
| Multi-account IMAP/SMTP, folders, HTML, attachments, notifications, compose and credentials | verified-all | not started | |
| Per-Space assistant, disclosures, approvals, cancellation, notes and persisted history | verified-all | not started | |
| Sync excludes cookies, passwords, extension storage, mail credentials and local paths | verified-all | not started | |
