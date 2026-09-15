# YOBRO Chromium migration plan

## Objective

Build a Chromium-based YOBRO for macOS, Windows, and Linux that matches the
current macOS WebKit application in appearance and behavior. The existing
WebKit application remains buildable and usable as a fallback until the
Chromium application has passed all parity and release gates.

The migration is a parallel product implementation, not an in-place engine
replacement. Both applications must use separate application identifiers,
process names, data directories, browser profiles, update channels, and build
pipelines.

## Non-negotiable requirements

1. Keep the current SwiftUI/AppKit/WebKit source and build scripts intact.
2. Do not let both applications write to the same profile or WebKit/Chromium
   storage directory.
3. Preserve the current YOBRO interaction model: sidebar, Spaces, folders,
   split tabs, compact mode, libraries, settings, onboarding, Mail, and the
   right-hand agent workspace.
4. Preserve the local agent protocol and its security semantics. Existing MCP
   clients should require no behavioral changes.
5. Treat the existing saved-state formats as migration inputs. Any format
   change requires a versioned, reversible migration.
6. Do not make the Chromium build the default until macOS parity, profile
   migration, recovery, and rollback have been verified with real user data.

## Recommended target architecture

Use Qt 6.10 or newer with QML/Qt Quick for the cross-platform UI and Qt
WebEngine for Chromium. Confirm the exact Qt version and extension API coverage
in the technical spike before committing the production branch.

```text
YOBRO UI (QML / Qt Quick)
        |
Application controllers
  tabs, Spaces, profiles, split view, settings, libraries
        |
Engine adapter
  Chromium pages, profiles, downloads, permissions, extensions
        |
Platform adapters
  macOS             Windows              Linux
  Keychain          Credential Manager   Secret Service
  Unix socket       Named pipe           Unix socket
  notifications     notifications        notifications
        |
Shared contracts
  persisted state, sync payload, agent protocol, import/export
```

The engine adapter is a hard boundary. Product code must not pass raw
`QWebEnginePage` or other Qt WebEngine objects into the UI and domain models.
This keeps the browser engine replaceable and makes the behavior testable.

## Repository and release layout

Keep the current files where they are during the migration. Add the new app in
a separate top-level directory rather than moving or renaming the working
WebKit implementation early.

```text
Sources/YOBRO/                 Existing macOS WebKit application
tests/Swift/                   Existing WebKit tests
scripts/build.sh               Existing WebKit build

chromium/
  app/                         Qt application entry points
  ui/                          QML screens and reusable controls
  core/                        Engine-independent browser/domain logic
  engine/                      Qt WebEngine adapter
  platform/macos/
  platform/windows/
  platform/linux/
  agent/                       Local control server and AgentBridge host
  extensions/                  Install, permissions, lifecycle, UI integration
  tests/

contracts/
  agent-protocol/              Versioned commands and response fixtures
  persisted-state/             Versioned schemas and migration fixtures
  sync/                        Versioned encrypted sync schema

tests/parity/                  Shared black-box behavior and visual fixtures
```

Recommended product identities during migration:

| Build | App name | Data directory | Update channel |
|---|---|---|---|
| Existing | YOBRO WebKit | Existing YOBRO directory | Stable/legacy |
| New | YOBRO Chromium Preview | New Chromium Preview directory | Preview |
| Final | YOBRO | New Chromium directory | Stable |

The Preview build may read the old profile only through the migration importer.
It must never mutate it. Retain the old app and its data until the user
explicitly removes them after a successful migration period.

## Feature-parity contract

The README and the existing test suites define the initial behavior contract.
Track every capability in a parity matrix with four states: not started,
implemented, verified on one platform, verified on all platforms.

### Browser shell and navigation

- Adaptive light/dark YOBRO theme and matching dimensions, spacing, typography,
  colors, hover states, animations, and focus behavior.
- Expanded and compact sidebars.
- Tabs, pinned tabs, colored folders, Spaces, drag reorder, closed-tab restore,
  duplicate tab, and split view.
- Start page, address/search flow, quick switcher, find in page, navigation,
  zoom, favicons, keyboard shortcuts, and window controls.
- Popups, permission prompts, certificates, downloads, media state, and crash
  recovery.

### Profiles and local data

- Separate persistent Chromium profile per YOBRO profile.
- Tabs, spaces, folders, bookmarks, history, downloads, settings, extensions,
  Mail, notes, and agent access state.
- Import from Safari, Chrome, Brave, Arc, Firefox, and the existing YOBRO WebKit
  profile.
- Password storage through the platform credential adapter.
- Explicit, previewed cookie/session migration; never silently copy sessions.
- Existing encrypted sync payload remains compatible or receives a versioned
  migration with old-reader protection.

### Extensions

- Local unpacked, ZIP, and CRX package installation.
- Permission preview and approval before activation.
- Enable, disable, remove, options pages, background workers, content scripts,
  toolbar actions, and popups.
- Per-profile installation and storage.
- Chrome Web Store package acquisition where legally and technically viable,
  without claiming compatibility before verification.
- A published compatibility matrix for required APIs and representative
  extensions.

### Agent collaboration

- Preserve protocol v2 commands and response shapes: status, tabs, open, new,
  focus, close, read, click, fill, scroll, navigation, pin, split, find,
  history, downloads, duplicate, and end.
- Preserve user-owned versus agent-owned tabs and the dedicated right pane.
- Preserve pause, profile/Space invalidation, single-command serialization,
  activity indicators, and opt-in library access.
- Port `AgentBridge.js` into Chromium's isolated application world.
- Maintain document-generation IDs and stale-element rejection.
- Traverse frames explicitly and keep password values redacted.
- Require post-action reads so the agent verifies observable page state.
- Keep arbitrary JavaScript and filesystem access out of the public protocol.
- Do not expose a permanent Chromium remote-debugging port. DevTools Protocol
  may be used in development or behind an authenticated, process-private bridge
  only if the public Qt APIs cannot provide a required capability.

### Mail, assistant, and sync

- Match multi-account IMAP/SMTP behavior, folder views, HTML reading,
  attachments, notifications, composing, and credential handling.
- Match the built-in per-Space assistant, disclosures, approvals, cancellation,
  notes, and persisted chat history.
- Keep sync engine-neutral and exclude cookies, passwords, extension storage,
  mail credentials, and local file paths.

## Delivery phases and exit gates

### Phase 0 — Freeze the reference (1–2 weeks)

- Produce a reproducible signed WebKit build and archive its source revision.
- Record supported macOS versions and a clean build procedure.
- Capture golden screenshots in light/dark mode at agreed window sizes.
- Turn the README feature list and existing tests into the parity matrix.
- Create sanitized fixtures for each persisted file and a realistic large
  profile.
- Record baseline cold start, idle memory, ten-tab memory, page-load CPU,
  energy use, and application size on Intel and Apple Silicon Macs.

**Exit gate:** A new developer can build the fallback and reproduce the visual,
functional, and performance baselines without touching a real user profile.

### Phase 1 — Chromium feasibility spike (2–4 weeks)

Build a disposable Qt prototype on macOS, Windows, and Linux containing:

- one persistent profile and multiple tabs;
- a minimal QML sidebar and split browser view;
- navigation, popups, downloads, permissions, and media playback;
- the current AgentBridge read/click/fill loop in an isolated world;
- same-profile login/cookie verification between user and agent panes;
- one cross-origin iframe test and one open-shadow-root test;
- installation tests for 10 representative extensions;
- frozen/discarded background-tab measurements.

**Decision gate:** Proceed with Qt only if all three platforms are distributable,
the agent loop is reliable, the required extension set is viable, and measured
resource use is acceptable. Otherwise compare CEF or a maintained Chromium fork
using the same tests before writing production UI.

### Phase 2 — Foundations (3–5 weeks)

- Establish CMake, dependency pinning, reproducible builds, linting, unit tests,
  crash reporting hooks, and artifact generation for all platforms.
- Define the engine interface and platform-adapter interfaces.
- Freeze protocol v2 fixtures and add cross-implementation contract tests.
- Define versioned persisted-state schemas independently of Swift `Codable`.
- Implement atomic writes, corruption quarantine, backups, and recovery.
- Establish separate Preview app IDs and directories.

**Exit gate:** CI produces installable Preview artifacts for all three systems;
unit and protocol contract tests pass without launching a web page.

### Phase 3 — Pixel-matched shell (4–7 weeks)

- Recreate the current window, theme, sidebars, Spaces, tabs, folders, panels,
  settings, onboarding, and keyboard behavior in QML.
- Replace SF Symbols with a licensed cross-platform icon set or owned vector
  assets while preserving visual weight and meaning.
- Add screenshot comparisons with small, explicitly reviewed tolerances.
- Add keyboard-only and screen-reader navigation tests.

**Exit gate:** Approved golden screens match on macOS, Windows, and Linux, and
the shell can be exercised with a fake browser engine.

### Phase 4 — Browser core and profiles (5–8 weeks)

- Implement tabs, navigation, history, bookmarks, downloads, zoom, search,
  split view, popups, permissions, certificates, proxies, ad blocking, media
  lifecycle, and crash restore.
- Implement one persistent Chromium context per YOBRO profile.
- Implement lazy session restoration and background-tab freezing/discarding.
- Verify that profile switching stops sensitive background work.

**Exit gate:** Browser and profile parity tests pass on all platforms, including
crash and forced-termination recovery.

### Phase 5 — Agent parity and hardening (3–6 weeks)

- Port the local control server to Unix sockets and Windows named pipes with
  owner-only permissions.
- Port the agent workspace and AgentBridge.
- Run the existing localhost integration scenarios against both engines.
- Add hostile-page tests for forged refs, navigation races, frame replacement,
  shadow DOM, oversized results, dialogs, downloads, and prompt injection.
- Add visible approval points for consequential actions without widening the
  MCP API.

**Exit gate:** The same protocol fixture suite and user journeys pass against
WebKit and Chromium; user-owned tabs remain inaccessible to agent commands.

### Phase 6 — Extensions (6–10 weeks)

- Implement installation, persistence, permissions, toolbar UI, popups,
  options, updates, and removal.
- Test representative categories: password manager, ad blocker, productivity,
  developer tool, wallet, shopping, media, and VPN/proxy.
- Publish known limitations by API and extension version.
- Add extension CPU/memory visibility and a safe-disable recovery mode.

**Exit gate:** The agreed launch extension set passes scripted and manual tests
on all supported systems. Unsupported capabilities fail visibly and safely.

### Phase 7 — Data migration, Mail, assistant, and sync (5–9 weeks)

- Implement a read-only importer for the existing YOBRO WebKit profile.
- Show a complete preview, counts, exclusions, and destination before import.
- Keep a migration journal and allow the new profile to be reset and reimported.
- Port platform credential storage, Mail, built-in assistant, and sync.
- Verify that secrets never enter logs, sync payloads, crash reports, or
  temporary files.

**Exit gate:** Sanitized and real opt-in migration rehearsals preserve all
supported data, leave the source untouched, and survive interruption.

### Phase 8 — Performance and release hardening (6–10 weeks)

- Measure against the Phase 0 WebKit baseline on identical hardware and pages.
- Set budgets for cold start, idle RAM, ten-tab RAM, background CPU, energy,
  package size, and update size.
- Optimize tab lifecycle without disabling Chromium sandboxing or site
  isolation.
- Run long sessions, profile switching, extension stress, offline, proxy,
  certificate, update, and rollback tests.
- Complete signing, notarization, Windows signing, Linux packaging, auto-update,
  privacy review, licenses, and incident/update procedures.

**Exit gate:** No release-blocking parity, migration, security, accessibility,
or data-loss defects remain, and performance budgets are met or explicitly
accepted.

### Phase 9 — Controlled rollout and fallback

1. Internal dogfood with synthetic profiles.
2. Opt-in macOS Preview alongside the WebKit application.
3. Small Windows/Linux alpha.
4. macOS migration cohort with automatic health and rollback checks.
5. Cross-platform public beta.
6. Chromium becomes the default download only after stability targets hold for
   an agreed observation window.
7. Keep the WebKit build downloadable and security-maintained for at least two
   stable Chromium release cycles.

The rollback path reopens the untouched WebKit application and profile. Data
created only in Chromium should be exported through the neutral sync/export
format; do not attempt a reverse copy of Chromium's live cookie database.

## Quality gates

### Visual parity

- Golden screenshots for every primary screen, both themes, expanded/compact
  sidebar, split view, empty/loading/error states, and German/English.
- Platform deviations require a recorded UX decision rather than accidental
  drift.

### Functional parity

- Shared black-box journeys run against both applications.
- Every README feature has an owner, automated coverage where practical, and a
  documented manual verification when automation is unsuitable.

### Agent safety

- No remote listening interface by default.
- Operating-system access control on every local IPC endpoint.
- No control API callable from web content.
- User tabs rejected unless a future explicit consent flow is designed.
- Secret fields redacted and sensitive libraries separately gated.
- Navigation invalidates references; actions are followed by observable reads.

### Performance

Set numeric budgets after the Phase 1 spike rather than guessing. At minimum,
measure p50 and p95 for startup and page load plus idle/active RAM, CPU, energy,
and renderer process count at 1, 10, 30, and 100 tabs.

## Principal risks

| Risk | Mitigation |
|---|---|
| Qt extension APIs do not cover launch requirements | Test real extensions in Phase 1; retain a CEF/fork decision gate |
| Chromium uses materially more memory/energy on macOS | Freeze/discard tabs, lazy restore, performance budgets, continuous comparison |
| Pixel parity drifts across platforms | QML design tokens, owned icons, golden screenshots, explicit platform exceptions |
| Existing data is damaged during migration | Separate directories, read-only source, preview, journal, atomic writes, fallback |
| Agent interface becomes too powerful | Narrow protocol, isolated script world, ownership checks, no public CDP port |
| Chromium security updates lag | Pin supported branches, automated update alerts/builds, defined emergency release SLA |
| Two applications confuse users | Preview branding, explicit migration status, clear default-browser prompts and rollback UI |

## Staffing and schedule estimate

For an experienced team of three to five engineers plus part-time design/QA,
expect roughly six to nine months to a credible public beta and nine to twelve
months to a polished stable migration. A single developer should plan for at
least twelve to eighteen months. Extension breadth, Mail portability, signing,
and Windows/Linux release engineering are the largest schedule variables.

These estimates assume that exact visual parity means the same YOBRO design and
interaction model, not identical native pixels where window controls, fonts,
menus, and accessibility conventions differ by operating system.

## Immediate next actions

1. Approve Qt WebEngine as the first spike candidate, not yet as the final
   irreversible choice.
2. Select ten must-support extensions and five must-support authenticated sites.
3. Define supported OS versions and CPU architectures.
4. Capture the WebKit golden screenshots and performance baseline.
5. Build the Phase 1 spike before beginning the production QML rewrite.

