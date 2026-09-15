# YOBRO Chromium

This directory contains the clean Chromium migration started after the discarded
UI-first prototype. The goal is full behavioral and visual parity with the
existing SwiftUI/AppKit/WebKit application while keeping that application and
its profile untouched.

## Current stage

**Phase 1D: packaged-controller and real engine lifecycle/policy feasibility.**
The executable is deliberately named `YOBRO Chromium Feasibility`; it is not a
product preview and does not claim UI parity.

The automated macOS-arm64 path now proves:

- an exact Qt WebEngine 6.11.2 / Chromium build;
- two isolated named persistent profiles plus an off-the-record context;
- user/agent cookie sharing only inside the same named profile;
- AgentBridge injection only on agent-owned pages in Qt's isolated application
  world, including negative user/private-agent checks;
- password redaction, stale-document rejection, open-shadow interactions,
  iframe-content exclusion, and file-upload rejection;
- 100 serial `read -> fill -> click -> read` bridge journeys;
- loopback navigation, redirects, history back/forward, and reload;
- the complete protocol-v2 controller and secure AF_UNIX transport contract;
- native Qt popup/opener adoption through a Qt-free synchronous host capability:
  `BrowserSession` owns every child before `QWebEngineNewWindowRequest::openIn`,
  inherits canonical User/Private/Agent ownership, privacy, space, and profile,
  never replays the requested URL, and handles `window.close()` through queued,
  instance-correlated removal;
- a real loopback popup matrix driven by `QTest::mouseClick`, with
  `event.source === window.open(...)` proving `window.opener`, persistent versus
  off-the-record profile identity, AgentBridge presence, inactive-profile
  rejection, and Agent-end teardown;
- a Qt-free, single-consumer permission capability with `BrowserSession` as the
  only grant authority; only an active, visible User tab with a mounted prompt
  presenter and canonical matching origin can request microphone, camera, or
  both, while Agent/background/inactive/hidden/package/unsupported requests and
  all stale lifecycle responses fail closed;
- an asynchronous WebKit-parity media dialog with deny as default/Escape,
  `AskEveryTime` persistent and private profiles, startup reset of legacy
  decisions, private User prompting without persistence, real fake-device
  audio/video grants, same-origin Agent isolation after a User grant, and
  repeated prompt evidence across identical-storage restarts;
- typed renderer recovery on the same `BrowserPage`, `QWebEngineView`, native
  page, and profile: one queued User/Private/background reload, five-second
  stability and crash-loop budget, explicit retry, immediate profile-
  reactivation ABA safety, session-owned Address/Back/Forward/Reload, and Agent
  fail-closed behavior without autoreload;
- a session-gated HTTP authentication prompt for the active visible User/Private
  tab, with same-origin and post-prompt lifecycle checks; Agent and proxy
  challenges fail closed, and a real Qt loopback HTTP Basic challenge completes
  through the prompt; real-site authentication parity remains open;
- a persistent-profile extension manager surface for unpacked MV3 installation,
  enable/disable, and removal, plus a real Qt MV3 content-script test covering
  execution, disable behavior, and install/activation restoration across two
  real application processes; toolbar popups, permissions, updates, and the ten-extension
  compatibility matrix remain open;
- deterministic no-`loadStarted`, stale-finish, watchdog, reentrancy, bounded-
  tombstone, exact-token/generation coverage plus real Qt renderer PID
  `SIGABRT`/`SIGKILL` evidence preserving history/DOM and producing exactly one
  active-load termination result and one recovery success; blocked Agent read
  and click operations return one nonempty error response;
- a strict profile-scoped `bridge-policy.json` store with fail-closed loading,
  corrupt-file quarantine, atomic owner-only writes, startup-before-socket
  application, live opt-in/opt-out, profile isolation, and a visible engineering
  shell toggle;
- a real Qt/AF_UNIX controller journey for status, tab ownership,
  read/fill/click, find, reload, library gate enable/disable, and end;
- real `QtBrowserLibrary` history with private-navigation exclusion, plus real
  fast/slow Qt downloads through start, partial progress, completion,
  cancellation, exact bytes/counters, isolated paths, persisted metadata, and
  user-prompt versus explicit-agent routing;
- a staged arm64 app containing Qt frameworks, `QtWebEngineProcess`, WebEngine
  resources, 53 locales, the Cocoa platform plugin, and versioned
  license/SBOM evidence; and
- an external packaged-app journey through the normal
  policy/session/controller/AF_UNIX object graph, including policy-at-readiness,
  history/download-list access, clean shutdown, and socket removal.

The public engine API in `engine/api/BrowserEngine.hpp` contains no Qt types.
Qt-specific classes stay in `engine/qtwebengine/`. Product UI must not receive
raw `QWebEnginePage` objects from the domain layer.

## Build and validate on macOS arm64

Requirements are pinned for this spike:

- CMake 3.25+
- Qt and Qt WebEngine 6.11.2 installed through Homebrew
- Apple Clang with C++20 support

```sh
./scripts/chromium-build.sh
```

That command performs a fresh Release/arm64 configuration, builds the app, runs
all registered controller, transport, policy, smoke, and real-Qt CTests,
deploys Qt into a clean staging directory, rewrites nested helper dependencies,
removes non-portable Homebrew rpaths, ad-hoc signs the bundle, and runs the
recursive package validator. The policy test covers strict fail-closed parsing,
quarantine, atomic owner-only persistence, profile isolation, and
commit-before-apply behavior. The real Qt popup test performs an actual rendered
mouse gesture and verifies opener identity, User/Private/Agent context
inheritance, native close, inactive-agent rejection, and Agent-end cleanup. The
real media test performs another rendered gesture and verifies the async
one-time prompt, deny and fake-track grants, private nonpersistence,
legacy-decision reset, hidden/package/background/Agent/inactive fail-closed
behavior, same-origin cross-owner isolation, repeated prompts after
identical-storage restarts, and a real HTTP Basic challenge through the visible
User login dialog. The renderer test combines a deterministic
load-signal state-machine harness with real Qt child-process `SIGABRT`/`SIGKILL`
termination, same-page/history recovery, crash-loop and immediate-reactivation
checks, exact-once active-load results, permission cancellation, and Agent
read/action fail-closed evidence. The real-Qt socket test includes live library
opt-in/opt-out, persisted history, private-history exclusion, download progress/completion/cancel, persisted
download records, and user-versus-agent prompt routing.

Validated output:

```text
dist/chromium-macos-arm64/YOBRO Chromium Feasibility.app
```

Run the package gate again without rebuilding:

```sh
python3 scripts/chromium-validate-package.py
```

The validator checks every real Mach-O file for arm64 and external dependencies,
required helpers/resources/locales, symlinks, ad-hoc signature integrity, and a
full packaged Phase-1B smoke with a clean `HOME`, profile root, and `PATH`. It
then prewrites an owner-only library policy, starts the staged executable with
its normal policy/session/controller/AF_UNIX object graph, requires the first
ready status to expose that opt-in, and externally verifies
`status -> history/downloads -> new -> download -> read -> fill/click -> read -> end`,
including the completed download's filename, byte counters, final path, and
exact file bytes, followed by clean process shutdown, persisted download
metadata and bytes, policy persistence, and control-socket removal against a
loopback-only fixture. It then restarts the packaged app with the same isolated
profile and verifies that the controller returns the identical completed
download before another clean shutdown.

Validate the shared engine-neutral contracts and the unchanged WebKit build:

```sh
python3 tests/parity/parity_runner.py
swift build
```

The contract gate validates every JSON contract, the compatible v1
history/download persistence schema and fixture, the required Boolean bridge
policy, protocol-v2 download lifecycle snapshots, and exact shared result/error
strings without launching a page.

Run the engineering shell with an isolated throwaway profile:

```sh
YOBRO_CHROMIUM_HOME="$PWD/.chromium-runtime" \
  open "dist/chromium-macos-arm64/YOBRO Chromium Feasibility.app"
```

The default data directory is `~/Library/Application Support/YOBRO Chromium
Feasibility` on macOS. `YOBRO_CHROMIUM_HOME` may override it for development.
The path policy rejects the WebKit home and equal/parent/nested paths. Library
access remains off for a missing or invalid policy and can be changed per
profile with the engineering-shell toggle.

## Packaging boundary

The staged bundle is self-contained for the tested local macOS-arm64 path and
is ad-hoc signed. It is **not** a public release: Developer ID signing,
hardened-runtime entitlement review, notarization/stapling, quarantine launch,
minimum-macOS compatibility, a clean second machine, and complete legal review
of the selected Qt license route remain open gates.

## Non-goals until the Phase-1 gate passes

No production sidebar, final Settings surface, Mail, Spaces, folders,
onboarding, visual parity claim, or release branding belongs in this spike.
Those are implemented against a fake engine only after Qt passes the remaining
extension, authentication, distribution packaging, performance, and
target-platform gates. Accepted and denied user-download prompts, download
failure/retry, restart recovery, and staging cleanup are covered by the real-Qt
download lifecycle test; the packaged protocol gate also verifies one complete
agent download through the staged app.
