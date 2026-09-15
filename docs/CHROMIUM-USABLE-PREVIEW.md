# YOBRO Chromium usable-preview milestone

## Target

The next deliverable is a locally usable macOS Apple Silicon Preview. It is a
separate application and profile, suitable for ordinary browsing and opt-in
testing without touching the WebKit profile.

## Final UI/UX parity goal

The end goal is a Chromium version whose user-facing macOS experience matches
the YOBRO WebKit application: the same sidebar-first information architecture,
visual hierarchy, spaces/folders/tabs workflows, page interactions and keyboard
journeys. Chromium remains a separate engine, profile and Agent implementation;
parity means equivalent visible behavior and interaction, never shared WebKit
state or weakened User/Agent isolation.

### Completion criteria

The final parity build is accepted only when all of the following have direct
evidence:

- **Product shell:** At 1440×900 and 1024×700 no engineering terminology or
  feasibility warning is visible. The sidebar holds navigation/address, Library,
  Space switching, pinned/folder/unfiled tabs, Agent state and profile access.
  Full, compact and autohide states return keyboard focus to web content and
  match approved reference geometry within 4 px.
- **Workspace:** Users can create, rename and select Spaces; assign icons; and
  create, rename, color, collapse, dissolve and reorder folders. Tabs can be
  renamed, pinned, duplicated, reordered, moved into folders or across Spaces,
  and dropped onto a compatible tab to split. Invalid names, Agent tabs and
  cross-Space split attempts are rejected visibly and safely. Every supported
  state restores after a restart.
- **Tab lifecycle:** Private tabs remain nonpersistent. Closing a public tab
  offers one six-second Undo and Reopen restores its Space, Folder, Pin and
  order. Two compatible User tabs can split and separate through UI and
  shortcuts; Agent tabs never appear in a User split or persistent workspace.
- **Navigation and keyboard:** Address suggestions include title, URL and visit
  count; typed input is never silently replaced by a suggestion. Cmd-L,
  Cmd-T, Shift-Cmd-T, Cmd-W, Cmd-1…9, Cmd-F, Cmd-D, navigation/reload, sidebar,
  Agent, split, zoom and print shortcuts are shown in product UI and covered by
  automated smoke tests. Page Find wraps, moves forward/backward and presents a
  no-match state.
- **Agent, Library and settings:** Agent content has a clear User-left/Agent-
  right presentation, ownership labels and a Pause/End path. The pane starts
  hidden and its toggle does not create or mutate an Agent page. Library,
  downloads and settings use the same in-shell hierarchy as WebKit; active
  downloads expose progress and safe actions without requiring a separate
  status dialog. Profile and settings entry points include the supported
  General, Privacy/Data, Appearance/Zoom, Extensions and Import workflows.
- **Visual and interaction system:** Every interactive control has focus,
  hover, pressed, disabled and loading feedback. The moss/paper/ink/orange
  system, content cards, icons, radii, spacing, dialogs, error surfaces and
  drag feedback match approved reference screenshots. Screenshot regressions
  cover light/dark appearance, shell/sidebar, address suggestions, folder DnD,
  split view, Agent, Library, download status and error states.
- **Evidence gate:** Clean-profile UI tests cover the criteria above together
  with User/Agent isolation and restart persistence. A 60-minute manual soak
  has no hanging dialog, focus loss, stuck drop target or ownership leak; the
  existing package, controller and security gates continue to pass.

### Delivery phases and estimate

1. **Parity foundation — 2–3 weeks:** replace engineering chrome with the
   WebKit-aligned sidebar-first shell, design tokens, responsive layout and the
   central shortcut/command routing.
2. **Workspace and tab parity — 5–7 weeks:** complete Workspace CRUD, context
   actions, drag/drop/reorder, closed-tab undo/reopen, tab metadata and User
   split view with persistent state.
3. **Navigation and surfaces — 4–6 weeks:** address suggestions, Page Find,
   page actions, Agent product surface, Library/Downloads presentation and
   Settings/Profile navigation.
4. **Visual completion and release evidence — 4–6 weeks:** screenshot and
   accessibility regression suites, responsive polish, clean-profile flows,
   soak evidence, hardened-runtime/signing and final package validation.

**Planning estimate:** **15–22 weeks of focused full-time engineering** after
the current technical baseline. This is a parity goal, not a claim that all
WebKit-only follow-up domains (Mail, Sync, the full built-in assistant,
platform expansion or public notarized distribution) become Chromium scope.

## Required before the milestone is complete

- Product-like browser shell with profiles, Spaces, tab folders, pinned and
  closed tabs, bookmarks, history, downloads, address/search, page search,
  split view, private tabs, keyboard shortcuts, and session restoration.
- Persistent cookies and website storage, HTTP authentication, popup login
  flows, media permissions, and renderer recovery.
- User/Agent ownership isolation and the complete protocol-v2 journey already
  established by the feasibility build.
- MV3 installation, listing, enable/disable, removal, and verified persistence
  across a real application restart. Extension failures remain visible.
- Read-only, previewed import from an existing YOBRO WebKit profile for the
  supported neutral data types. The source remains unchanged and reimport is
  repeatable.
- Reproducible Apple Silicon package, preview identity, hardened runtime and
  local signing, clean-profile package validation, crash-free soak evidence,
  and documented known limitations.

## Explicit follow-up scope

Mail, sync, the built-in assistant, the full ten-extension compatibility
matrix, Intel macOS, Windows, Linux, Developer ID notarization, auto-update, and
public rollout are not required for this first usable Preview. They remain part
of the broader Chromium roadmap.

## Current evidence

- Qt WebEngine 6.11.2 feasibility bundle builds, and the Chromium CTest suite
  passes 12 of 12 controller, transport, policy, popup, permission/authentication,
  MV3, renderer, download, packaged-protocol, and WebKit-import tests.
- The packaged Apple Silicon app is self-contained and its isolated download
  and persisted library state survive a real restart.
- Installed MV3 files and YOBRO's profile-scoped activation choice now survive
  a verified two-process restart and execute again on the fixture page.
- The read-only WebKit import is now usable from the Chromium Preview toolbar:
  a user selects a WebKit profile directory, receives a non-mutating preview,
  explicitly confirms the import, and receives an outcome in the active profile.
  Filtered history and bookmarks are merged into the visible Chromium library;
  imported HTTP(S) tabs open immediately and restore after restart, and source
  data plus the profile import journal remain available for inspection.
- The Preview now has a profile-local library window with search, visible
  history and bookmark lists, open-in-new-tab behavior, bookmark removal, and
  a direct bookmark action for the current public page. Bookmarks use the same
  bounded JSON, corruption quarantine, atomic write, and owner-only permission
  rules as the existing history and downloads records.
- Session format v2 persists Space names, tab-folder definitions, active Space,
  and public-tab Space/folder/pinned metadata. It accepts existing v1 URL-only
  sessions; private and Agent tabs remain excluded from persisted workspace
  data. The toolbar provides Space switching/creation, folder creation, and
  explicit placement of the active tab into a folder.
- The end-to-end Qt test verifies preview, explicit confirmation, untouched
  source files, library visibility and persistence for imported history and
  bookmarks, import journal, tab restoration, and v2 Space/folder restoration.
  The MV3 restart harness now retries only transient child-process startup or
  shutdown failures and reports each failed attempt.
- The Chromium shell now projects the active persistent Space into a product-like
  left workspace sidebar. It presents pinned tabs, per-Space folder groups, and
  unfiled tabs; selecting a tab activates it through `BrowserSession`, while
  private and Agent tabs remain outside persisted workspace data. A visible pin
  control persists the existing v2 `pinned` metadata. Folder groups can be
  collapsed, their `collapsedFolders` state is atomically persisted in the v2
  session and restored after restart. The isolated Agent pane is hidden by
  default and can be shown or hidden from an explicit shell toggle without
  creating, ending, or changing any Agent-owned page.
- The Qt integration suite directly verifies sidebar rendering, pinned and
  folder-group restoration, persisted folder collapse, and the Agent-pane UI
  toggle. Its WebEngine renderer-recovery media click now drains the event loop
  after focus assignment, matching the established permission-test interaction
  pattern; the real recovery test passed three consecutive runs.
- A fresh arm64 Release build with pinned Qt 6.11.2 passed all 12 CTests and
  the staged package gate. The clean-profile packaged Protocol-v2 journey,
  persisted policy/download restart, recursive Mach-O linkage/signature checks,
  53 locale resources and 41 Mach-O files passed for the 288.4 MiB bundle.
- The current shell has product-facing library and workspace navigation, but
  still needs page find, split view, richer workspace interaction (reorder,
  rename/delete operations and cross-Space drag/drop), a broader visual product
  pass, soak testing, hardened-runtime/release signing and final distribution
  validation.

## Working estimate

Assuming focused development on the existing codebase and no Qt API blocker:

1. Product shell and core browsing parity: 3–5 weeks.
2. Import and extension restart persistence: 1–2 weeks.
3. Packaging, hardening, regression work, and dogfood fixes: 2–3 weeks.

Expected time to a locally usable Apple Silicon Preview: **6–10 weeks of
full-time engineering work**. A first internal dogfood build should be possible
in roughly **3–4 weeks**. The estimate grows if the UI must match the WebKit app
pixel-for-pixel before dogfood or if Qt extension limitations require engine
workarounds.

The public cross-platform release remains materially larger: approximately
**4–8 additional months**, plus access to Windows/Linux test machines, Apple
Developer ID credentials, notarization, extension-account testing, and a beta
observation period.

## Completion rule

The milestone is complete only when every required item above has direct test
or package evidence and a clean-profile user can browse, restart, import data,
manage extensions, and use the Agent pane without the WebKit application.
