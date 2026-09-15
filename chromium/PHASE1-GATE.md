# Phase 1 — Chromium feasibility gate

Qt WebEngine is a candidate, not an irreversible product choice. Every item
must have reproducible evidence before the pixel-matched YOBRO shell begins.

## Evidence snapshot — 2026-09-13

Current decision: **UNDECIDED**. Phase 1B, the local macOS/Unix Phase 1C
protocol slice, and the packaged-controller, real Qt library/download, persisted
library-policy, popup/opener ownership, permission/media, and renderer-recovery
Phase 1D increments pass on macOS arm64. Mandatory cross-platform, remaining
lifecycle/policy, extension, authenticated-site, release, and performance
evidence is incomplete.

### Proven locally

- `./scripts/chromium-build.sh` completed from a fresh Release/arm64 CMake tree
  against pinned Qt WebEngine `6.11.2`. The smoke reports Chromium
  `140.0.7339.225` (security-patch version `151.0.7922.71`).
- The fresh pipeline ran all eight CTests successfully (`8/8`, `63.85 s`):
  - `chromium-protocol-v2-controller` covers strict bounded JSON, exact
    envelopes and dispatch precedence, busy exceptions, ownership, correlated
    navigation completion/timeouts, all advertised commands, dynamic library
    opt-in/opt-out and status, download/cancel behavior while list access is
    disabled, and `end` cancellation/lifecycle behavior. Its engine-neutral
    popup matrix additionally freezes authoritative User/Private/Agent
    inheritance, canonical ID/owner repair after untrusted engine state,
    provisional-adoption rollback, inactive-profile and disabled-agent
    rejection, throwing-observer commit safety, queued/double/ABA-safe close,
    close-after-session-teardown safety, and Agent-end cleanup.
  - `chromium-protocol-v2-unix-socket` covers the exact 1 MiB-before-LF limit,
    oversized/missing-LF rejection, partial I/O, read/write timeouts, owner-only
    parent and `0600` socket permissions, peer UID, fail-closed path handling,
    stale-socket/inode-safe cleanup, and controller dispatch over AF_UNIX.
  - `chromium-bridge-policy` covers missing-policy default `false`, canonical
    owner-only atomic true/false writes and reload, compatible extra keys,
    malformed/missing/wrong-type/duplicate/root/oversize rejection with
    `.corrupt[-N]` quarantine, profile isolation, commit-before-apply ordering,
    and no runtime mutation after a write failure.
  - `chromium-phase1b-smoke` covers profile/cookie isolation, AgentBridge
    boundaries, 100 serial read/fill/click/reread journeys, and loopback
    navigation/history/reload/redirect behavior offscreen with sandbox and site
    isolation defaults. No opt-out flag or permanent remote-debugging port is
    present.
  - `chromium-phase1d-popup-ownership` uses loopback parent/child documents and
    a real `QTest::mouseClick` delivered to Chromium's rendered widget. The
    parent accepts only a same-origin message whose `event.source` equals the
    exact handle returned by `window.open`, proving that native
    `QWebEngineNewWindowRequest::openIn` preserves `window.opener` rather than
    replaying a URL. Persistent User, off-the-record User, and Agent popups are
    session-owned and inherit immutable owner, privacy, space, and the exact
    opener `QWebEngineProfile`; the persistent and private profiles remain
    distinct. Native `window.close()` removes only the child through queued
    instance-correlated close, the AgentBridge exists in an adopted Agent
    child, Agent `end` removes opener and child, and an inactive profile rejects
    Agent popup creation. GPU disabling is test-only.
  - `chromium-permission-media` drives a loopback secure-context fixture through
    a real rendered `QTest::mouseClick`, the native Qt 6.11
    `permissionRequested` signal, the session authority, and a non-blocking
    `QMessageBox::open` prompt. The prompt reproduces the WebKit host/device and
    page-visit-only text, exposes only “Einmal erlauben” and “Ablehnen”, and
    defaults/Escape to deny while the Qt event loop continues. Explicit deny is
    observed by JavaScript; explicit grant produces and then stops fake audio
    and video tracks without bypassing the application prompt. Persistent and
    private profiles are both `AskEveryTime`; a seeded legacy on-disk
    geolocation grant is reset on profile startup; persistent media denial and
    grant each prompt again after separate restarts using the identical storage.
    Private User media remains promptable but off-the-record. Background User,
    Agent, inactive-profile, hidden-surface, hidden packaged-harness, mismatched
    origin, unsupported geolocation, missing/throwing presenter, concurrent
    request, timeout, navigation, renderer, close, active-tab change, popup
    rollback, and teardown paths fail closed. A same-origin Agent page cannot
    inherit a real User media grant even though both use the named profile. The
    engine-neutral tests additionally prove one-shot resolution, monotonic
    tab-instance/ABA binding, document-epoch and canonical-origin revalidation,
    and stale-response rejection. GPU disabling and fake media devices are
    test-only; fake UI permission bypass is deliberately not enabled.
  - `chromium-renderer-recovery` combines a deterministic Qt lifecycle peer,
    engine-neutral controller cases, and real renderer child processes. The
    deterministic matrix covers no-`loadStarted` success/failure, a bounded
    five-second watchdog, late-signal inertness, terminate-A/retry-B stale
    finishes in both ambiguous orders, exact token/generation completion,
    callback reentrancy, and the eight-entry tombstone bound. The controller
    matrix covers all four Qt termination kinds, one delayed User/Private/
    background reload on the same `BrowserPage`/view/page/profile, a five-second
    stability budget, visible second-crash failure, explicit retry, permission
    denial, immediate profile deactivate/reactivate with late state/result,
    direct-link plus session-owned Address/Back/Forward recovery, tab close,
    and Agent fail-closed read/find/action/scroll deadlines.
    The real loopback test terminates Qt renderer PIDs with `SIGABRT` and
    `SIGKILL`, preserves URL/history/DOM and the exact page/view/profile
    identities, verifies Back after recovery, proves exactly one
    `rendererTerminated` result for an active load and exactly one successful
    recovery result in native signal order, rejects crash loops until manual
    retry, cancels media authority, keeps profile-cancelled recovery failed
    across immediate reactivation, and returns `ok:false` with a nonempty error
    exactly once for blocked Agent read and click operations without autoreload.
    Sandbox/site-isolation defaults remain enabled; GPU disabling is test-only.
  - `chromium-protocol-v2-qt-socket` drives a real Qt WebEngine page through the
    controller and AF_UNIX socket for status, user-tab rejection,
    new/read/fill/click/read, find, reload, tabs, and end. It starts without a
    policy and proves that `history` and `downloads` are blocked, enables access
    through the same GUI-thread commit-then-apply path used by the feasibility
    shell, verifies status and `bridge-policy.json`, executes the real library
    journey, then disables access and verifies status, persisted `false`, and
    both gates again. Its real `QtBrowserLibrary` path proves history IDs,
    dates, visits, and private-navigation exclusion; a fast download through
    `completed` with exact 19-byte content, counters, name, and isolated
    destination; an 8 MiB slow download with observable partial progress,
    cancellation, terminal `cancelled`, and exact inactive/unknown cancellation
    errors; and persisted completed/cancelled records in `downloads.json`.
    Explicit agent downloads bypass the user prompt, while a queued user-page
    download invokes the prompt callback exactly once with its name/directory
    and, when rejected, is not persisted. `end` removes the agent tab. The
    deterministic offscreen test disables GPU; this is a test-only environment
    setting.
- The profile-scoped `BridgePolicyStore` is loaded before `BrowserSession`, the
  protocol controller, and the local socket start. It accepts only a bounded,
  duplicate-free JSON object with Boolean `allowsLibraryAccess`, fails closed,
  quarantines invalid files, rejects symlink/non-file endpoints, and atomically
  commits owner-only files. The feasibility shell exposes a visible per-profile
  toggle; a failed commit leaves both the toggle and session at their previous
  state. The `BrowserSessionConfig` default remains `false`.
- The protocol implementation uses one authoritative `BrowserSession` for UI
  and controller ownership, correlated `NavigationToken` completion,
  finish-once timers, and RAII engine event subscriptions. Agent `end` removes
  agent pages rather than transferring isolated bridge state to user ownership.
- The engine-neutral new-window capability exposes only a synchronous
  `openIn(BrowserPage&)`, never a requested URL. `BrowserSession` creates and
  owns the child before native adoption, derives owner/private/space only from
  immutable opener metadata, transactionally rolls registration back on
  rejection, and prevents engine state from rewriting canonical ID or owner.
  Qt accepts only a different `QtBrowserPage` using the identical
  `QWebEngineProfile`. Close requests are queued outside the Qt signal stack and
  correlated by monotonic tab-instance identity, so duplicate, stale-ID, and
  post-session tasks are harmless; observer failures cannot roll back committed
  ownership.
- The permission boundary is also Qt-free and single-consumer. `QtBrowserPage`
  maps only microphone, camera, and their combination into an RAII capability;
  unsupported native types and every allocation/conversion/consumer exception
  deny before leaving Qt signal dispatch. `BrowserSession` alone can grant and
  requires an active profile, an explicitly mounted prompt surface and
  presenter, canonical same-origin identity, a nonloading active User tab, the
  same tab instance/document epoch/URL at response time, and an explicit user
  answer. The native capability is removed from the pending slot before
  resolution, is denied exactly once on drop/cancel/timeout/teardown, and cannot
  be exposed to the shell. The package protocol harness explicitly disables
  this surface despite constructing the normal `SpikeWindow` object graph.
- Renderer recovery is session-owned and retains the same page, Qt view, native
  page, profile, history, opener context, and AgentBridge installation. User
  pages receive one queued automatic reload and renew that budget only after a
  stable success window; a second pre-stability termination exposes the
  WebKit-parity error and requires explicit retry. Agent pages never autoreload.
  Every navigation, callback, recovery task, and cancelled result is bound to
  tab instance, document epoch, token, and generation. The shell routes Address,
  Back, Forward, and Reload through `BrowserSession`; fresh native link loads
  explicitly take ownership. Profile cancellation leaves bounded token
  tombstones, so delayed recovery state/results cannot change health, history,
  protocol waits, or visible failure after reactivation.
- Two named persistent profiles have distinct storage/cache paths and cookie
  jars. User and agent pages in one profile share its cookie; a second named
  profile and the off-the-record profile do not.
- Agent-owned off-the-record creation is rejected. User-owned pages reject
  snapshots/actions before JavaScript execution and have no AgentBridge in
  `QWebEngineScript::ApplicationWorld`.
- The shared bridge proves password redaction, open-shadow `fill`/`click`,
  iframe metadata-only exclusion, file-input rejection, reread, document-ID
  rotation, stale-document rejection, BFCache identity rotation, and fresh
  remapping of reattached elements.
- The current staged artifact at
  `dist/chromium-macos-arm64/YOBRO Chromium Feasibility.app` passed the package
  gate with 41 arm64 Mach-O files, no detected external non-system dependency
  or rpath, `QtWebEngineProcess`, ICU/V8/PAK resources, 53 locales, the Cocoa
  plugin, unbroken symlinks, license/SBOM evidence, deep/strict ad-hoc signature
  verification, and a clean-environment packaged Phase 1B smoke. Logical
  payload size is `288.2 MiB`.
- The same package gate also launches that staged executable in an isolated,
  owner-only short-path profile using the normal
  profile/policy/library/`BrowserSession`/`ProtocolV2Controller`/AF_UNIX/
  `SpikeWindow` object graph. A test-only presentation/lifecycle option keeps
  the Cocoa window off-screen and lets the external parent request clean exit
  over stdin; it does not replace controller or protocol behavior. The external
  validator prewrites an owner-only `bridge-policy.json`, requires the first
  identity-checked `status` to report `libraryAccess=true`, verifies real
  `history` and `downloads` list access, then proves
  `new -> read -> fill -> click -> read -> end -> tabs` against a loopback-only
  fixture, dynamic document/ref values, agent ownership, agent-tab removal,
  exit code zero, control-socket unlink, and unchanged owner-only policy
  persistence. The journey still does not transfer an actual file.
- `python3 tests/parity/parity_runner.py` passes. It parses every contract JSON
  file and semantically checks the v1 history/download schema and fixture,
  including WebKit numeric and Chromium ISO persisted dates, the complete
  download field/state contract, nullable optional path/error values, and the
  required Boolean bridge-policy field. It also freezes protocol-v2 download
  start, progress, completion, cancel, and cancelled snapshots; ISO wire dates;
  the success hint; and invalid-URL, unknown-download, and inactive-download
  errors against both implementations. It remains a no-page contract gate, not
  an engine run.
- `swift build` passes, confirming that the WebKit product remains buildable
  after the shared contract changes.

### Explicitly not yet proven

- macOS x86_64, Windows x64, and Linux x64 builds have not run. The secure local
  transport is currently implemented and tested for Unix; the Windows
  named-pipe implementation and its owner-only ACL contract remain behind the
  transport seam.
- Not every protocol command has been exercised through a real Qt page, and
  there is no fixture-driven all-command run against both WebKit and Chromium
  engines.
- The engineering shell policy toggle is functional, but the eventual
  pixel-matched Chromium Settings surface remains outside this feasibility
  phase.
- Download evidence does not yet include the visible confirmation UI or an
  accepted user download, real Qt failure/`DownloadInterrupted` handling,
  restart conversion to `interrupted`, private-profile downloads, partial-file
  cleanup, collision/overwrite policy, an active transfer during `end` or app
  shutdown, or an actual file transfer in the external packaged journey.
- Real extensions and App Store Connect, Gmail, GitHub, iCloud Web, and banking
  authentication flows have no recorded evidence.
- The app is not Developer-ID signed or notarized; hardened-runtime entitlement
  review, minimum-macOS, quarantine/Gatekeeper launch, clean second machine,
  updater channel, distribution artifact, and complete legal review remain
  open.
- Startup/page-load percentiles, tab-scale RAM/CPU/energy/renderer counts,
  freeze/discard behavior, and the 30-minute idle run have not been measured.

## Engine and isolation

- [ ] Exact Qt/Chromium versions build on macOS arm64/x86_64, Windows x64, and Linux x64.
- [ ] Preview app ID, process name, update channel, and profile home are separate from WebKit.
- [x] Persistent user and agent pages share cookies; private and second-profile pages do not.
- [x] Local arm64 navigation, history, reload, redirect, and correlated completion work.
- [x] Local real Qt completion, partial progress, cancellation, metadata persistence, and agent/user-prompt routing work for downloads.
- [x] Profile-scoped library opt-in is fail-closed, owner-only, atomic, loaded before socket readiness, and live-reversible.
- [x] Real Qt popup/opener ownership preserves native `window.opener`, opener profile/owner/privacy/space inheritance, queued close, and Agent-end cleanup.
- [x] Active visible User camera/microphone prompts, private nonpersistence, canonical origin, one-shot/ABA/lifecycle cancellation, hidden/package/Agent fail-closed behavior, and restart/legacy-reset evidence work.
- [x] Typed renderer termination, same-page one-shot recovery, stability/crash-loop budget, immediate profile-cancellation ABA safety, Agent fail-closed behavior, and deterministic plus real PID/signal evidence work.
- [ ] Accepted user-download UI and failure/restart/cleanup download cases work.
- [x] Sandbox and site isolation remain enabled; no permanent remote-debugging port exists.

## Agent contract

- [x] AgentBridge runs only in an isolated application world on agent-owned pages.
- [x] The Qt-free controller and Unix transport preserve frozen protocol-v2 envelopes, ownership, limits, errors, and timeout behavior deterministically.
- [x] A real Qt/AF_UNIX `read -> fill/click -> read` journey preserves document/ref behavior.
- [x] Real Qt popups preserve opener identity and authoritative User/Private/Agent ownership; queued close and Agent end retain session lifecycle boundaries.
- [x] A real Qt/AF_UNIX library journey preserves opt-in/opt-out, history privacy, and download start/progress/completion/cancel/persistence/prompt boundaries.
- [x] The staged app loads persisted library policy before socket readiness and its normal controller/socket graph passes an external clean-profile journey and clean shutdown.
- [ ] Every command is fixture-driven through both real engines and every target-platform transport.
- [x] Password values are always redacted and stale document references are rejected.
- [x] Open shadow roots and the agreed metadata-only iframe behavior are covered.
- [x] User-owned tabs and file-upload fills remain inaccessible to agents.
- [x] Dialog and media-permission flows remain inaccessible without an active visible User surface and explicit one-time user authorization.
- [x] 100 repeated local journeys complete without ownership or navigation races.

## Extensions and authenticated sites

- [ ] Each extension in `docs/CHROMIUM-SCOPE.md` is tested at a recorded version and hash.
- [ ] Installation, enablement, restart persistence, popup/options, content scripts, and its critical workflow work.
- [ ] App Store Connect, Gmail, GitHub, iCloud Web, and the selected 2FA banking flow have manual evidence.
- [ ] No credentials, cookies, tokens, or page content appear in logs or fixtures.

## Distribution and performance

- [x] The local arm64 artifact includes the Qt WebEngine helper, runtime resources, 53 locales, the Cocoa plugin, and versioned license/SBOM evidence.
- [ ] Developer ID, hardened-runtime entitlements, notarization, quarantine launch, minimum macOS, second-machine execution, and complete license review pass.
- [ ] p50/p95 startup and page-load times are recorded.
- [ ] RAM, CPU, energy, renderer count, and package size are recorded at 1/10/30/100 tabs.
- [ ] Freeze/discard behavior and a 30-minute idle run meet pre-agreed budgets.

## Decision

- `PASS`: Qt satisfies all mandatory gates; proceed to foundations and then the fake-engine pixel shell.
- `NO-GO`: run the same harness against CEF.
- `UNDECIDED`: missing platform, extension, auth, or performance evidence is not a pass.

Current result: **UNDECIDED**. The local macOS/Unix Phase 1C controller,
transport, ownership, and command increment plus the packaged-controller, real
Qt library/download, persisted library-policy, native popup/opener ownership,
and fail-closed permission/media plus renderer-recovery Phase 1D increments are
implemented and reproducible. The next evidence increment is accepted
user-download confirmation and download failure/restart/cleanup cases. Windows
transport, real extensions/authenticated sites, all target platforms, release
signing/notarization, and performance also remain mandatory. Pixel matching
still waits for a full Phase-1 `PASS`.
