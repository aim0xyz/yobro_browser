# YoBro

**Your browser. Your bro.**

A native macOS browser prototype with an Arc-inspired sidebar and direct agent access to **the same visible tabs**. SwiftUI + AppKit + Apple's WebKit. No Chromium, Electron, Playwright, browser extension, MCP server, or computer-use automation.

## Run

Requires macOS 14+, Xcode command-line tools, and Python 3 for the optional CLI. No third-party packages.

```sh
./scripts/build.sh
open dist/YoBro.app
./bin/yobro status
./bin/yobro new https://example.com
./bin/yobro read
```

The built app is at `dist/YoBro.app`. The CLI is also included in the app at `Contents/MacOS/yobroctl` (a distinct filename from the internal `YOBRO` executable on case-insensitive macOS volumes). The app itself does not require Python.

## Browser

YoBro uses German when the primary macOS language is German (including Austria and Switzerland), and English for all other languages. The language is selected at launch and covers menus, settings, onboarding, mail and import messages. Existing user-created names and imported content are preserved.

The first-run onboarding offers an optional browser-data import with preview and selection. Its completion is saved locally; reopen it via **Browser → Einrichtung erneut öffnen**.

Passkey signing preparation and remaining NordVPN limitations: [implementation status](docs/PASSKEYS-NORDVPN.md).

**Erweiterungen & Import:** Install compatible Chrome Web Store packages or local WebExtensions (macOS 15.4+), import selected bookmarks, history, tabs, password CSVs and cookies from supported browser profiles or exports, and manage imported bookmarks and Keychain passwords via **⌘,**. See [formats and limitations](docs/EXTENSIONS-IMPORT.md).

**YoBro Mail:** the sidebar envelope now opens an internal multi-account IMAP/SMTP client with provider discovery, unified inbox, all IMAP folders, HTML reading, attachment downloads, read-status updates, optional notifications and composing. Gmail/app passwords, iCloud Mail, Spacemail and generic server configuration are supported paths; OAuth-only accounts are not yet connectable. See [Mail setup and current limitations](docs/MAIL.md). The mail transport currently requires local Python 3.9+.

YoBro follows the macOS light/dark appearance automatically, including changes while running. The native sidebar, start page, library, search and agent panels use adaptive colors. Websites retain their own styling; websites implementing `prefers-color-scheme` can respond to the system appearance. No forced page inversion is applied.

Cross-device history sync is deferred. The intended future scope includes Android, iOS, macOS, Windows and Linux, so an iCloud-only solution is not planned. No backend has been created and no history is uploaded.

- Native WebKit tabs with persistent cookies and login sessions. New tabs immediately focus the search field on the start page, without a popover. ⌘L also focuses this field on blank tabs; on websites it opens the address editor.
- Collapsed sidebar keeps mail, spaces and tabs as icons; history and downloads stay at the bottom.
- Sidebar, pinned tabs (right-click a tab), custom spaces with selectable icons, colored tab folders and resizable split view.
- Compact controls without a permanent URL field; ⌘L opens address/search (DuckDuckGo).
- New-tab page, launch shortcuts, persistent tab URLs and selection.
- Built-in per-Space agent chat with a persisted timeline, email context, saved notes, visible tab reads and approved URL opening. Configurable Chat Completions endpoint; local agent access retains its pause switch.
- Searchable local history, and a quick switcher for tabs, history and web search.
- WebKit download manager with progress, cancellation, Finder reveal and collision-safe filenames.
- Drag a tab onto another tab to pair them in split view; separate them without closing either page. Drop near a row edge to reorder. Website favicons appear beside tab titles.
- Duplicate tabs and reopen the last 20 closed tabs, including after a restart.
- Native in-page search with next/previous match and wraparound.
- ⌘S toggle sidebar, ⌘T new tab, ⌘L address, ⌘W close tab, ⌘R reload, ⇧⌘S split, ⇧⌘A agent panel.
- ⌘K quick switcher, ⌘F page search, ⌘G / ⇧⌘G next/previous match, ⌘Y history, ⇧⌘J downloads, ⇧⌘T reopen closed tab.

Spaces organize tabs within a profile and share that profile’s cookie store. Local browser profiles have separate cookies and website data. Sessions restore URLs, not back/forward history or in-page form state.

## Local browser profiles

The avatar at the bottom left opens profile switching, **New profile**, **Edit profile**, Settings and browser-data import. Profiles have a name and selectable icon; no registration or cloud account is required. The compact sidebar has the same menu.

The existing browser becomes **Personal** without moving its files or resetting its WebKit store. New profiles start empty and get separate persistent WebKit stores, tabs/spaces, history, bookmarks, imported Keychain passwords, extensions, mail-account lists and download histories. The selected profile is restored on launch. Profile names and icons can be changed without changing storage identifiers.

Registry: `profiles.json` in the existing `YOBRO` data directory. Additional profile files: `Profiles/<UUID>/`. Additional profiles also keep downloaded files in their own `Downloads/` subdirectory. New profiles start with agent access paused. Inactive profiles reject agent requests and stop mail polling; media playback is paused on switching. Existing downloads and website/extension background activity may continue within their original profile. Profiles are separate browser contexts, not macOS user accounts or locked vaults. Language and the app’s chrome appearance remain app-wide. Profile deletion and custom photo uploads are not included yet.

## Agent protocol

### ChatGPT and Codex plugin

The local `yobro-browser` plugin packages the agent protocol as MCP tools plus an automatically discoverable workflow. After installing it from the repository marketplace, prompts such as **“Use YoBro to look this up”** or **“Open this in YoBro”** route browser work to the running YoBro app instead of a separate browser runtime.

The MCP adapter connects directly to `~/Library/Application Support/YOBRO/control.sock`. Set `YOBRO_SOCKET` only when targeting the explicit socket shown by another YoBro profile's Agent panel. The adapter does not open a TCP listener, copy cookies, or expose the socket to web content.

```sh
codex plugin marketplace add /absolute/path/to/Orbit
codex plugin add yobro-browser@personal
```

Restart the ChatGPT desktop app and start a new chat after installation so the plugin's skill and MCP tools are loaded. The plugin source is in `plugins/yobro-browser`; its dependency-free smoke test is:

```sh
python3 plugins/yobro-browser/scripts/test_yobro_mcp.py
```

Agent protocol 2 uses a dedicated right-hand pane. The first page command opens it automatically; the user's active tab and address bar stay on the left. `tabs` identifies each tab with `owner` (`user` or `agent`) and `agentActive`. Commands without `--tab` target the agent's current tab. User tab IDs are rejected: use `new URL` to open a separate page in the agent pane. This reloads the URL; it does not clone unsaved form state. The panes share the profile's cookies and website storage.

The right header shows Working / Ready, a menu of agent tabs, and Pause. Pause disables access, closes the pane and returns agent tabs to the normal tab list. Space/profile switching also pauses the agent. `end` closes the agent session and returns its tabs without disabling future access. Agents should call `end` when finished. Concurrent commands are rejected while another command is running. `space`, `panel`, `restore`, and `move` are unavailable through the agent protocol because they affect the user's workspace. Status and tab listing do not open the pane. JavaScript dialogs from agent pages are declined instead of blocking the user's window; pause and take over the tab if a dialog requires user action.

Additional profiles use `p-<UUID>.sock` in the root `YOBRO` data directory. Copy the exact path from that profile’s Agent panel, then use `./bin/yobro --socket PATH status` or `YOBRO_SOCKET=PATH ./bin/yobro status`. The default CLI stays attached to the original profile: it never silently follows a profile switch. Only the active profile accepts requests.

The original profile listens on a Unix-domain socket at `~/Library/Application Support/YOBRO/control.sock`. The directory is private, the socket has mode 0600, and the server checks the connecting user's UID. There is no TCP port. A connection carries one newline-terminated JSON request and receives one JSON response. Local processes running as your user can use this interface while enabled. Web content cannot connect to it. Pause access in the Agent panel when needed.

```sh
./bin/yobro tabs
./bin/yobro open https://example.com
./bin/yobro read
./bin/yobro --tab TAB_UUID read
./bin/yobro click e1 --document DOCUMENT_UUID
./bin/yobro fill e2 'Some text' --document DOCUMENT_UUID
./bin/yobro scroll 600
./bin/yobro pin
./bin/yobro split
./bin/yobro find 'search text'
./bin/yobro history 'github' --limit 20
./bin/yobro download https://example.com/report.pdf
./bin/yobro downloads
./bin/yobro cancel-download DOWNLOAD_UUID
./bin/yobro duplicate
./bin/yobro end
```

`read` returns text, headings, interactive elements and a document UUID. Use element refs and the document UUID from this response for `click` / `fill`. References are kept stable within one document. Navigation invalidates them. Read again after actions to verify the actual outcome; dynamic pages can update asynchronously. `open`, `reload`, `back` and `forward` wait up to 20 seconds for loading. `new` returns the new tab ID.

JSON example:

```json
{"command":"fill","tab":"TAB_UUID","document":"DOCUMENT_UUID","ref":"e2","value":"Some text"}
```

Send arbitrary supported protocol requests through `./bin/yobro raw` with JSON on stdin. The control API intentionally does not provide arbitrary JavaScript execution or arbitrary filesystem access. Downloads write only into YoBro's download directory and return their final path once complete.

Downloads use the initiating WebKit session, including its cookies. Files are stored under `~/Downloads/YOBRO`; test sessions with `YOBRO_HOME` use `YOBRO_HOME/Downloads`. Transfers use a hidden temporary file, then move to the final filename after completion. Existing files are never overwritten. Cancellation and failure remove the partial file; completed files are never automatically opened. Progress and history are visible in the Downloads library. Interrupted downloads are marked on restart; resumable downloads are not yet implemented.

`find` uses native WebKit text search and returns `found`, not a total match count. `history` reads saved visits, and `panel` opens `history`, `downloads`, `palette`, `agents`, or closes panels with `none`. History and closed tabs are local, shared across spaces, and included in the access controlled by the Agent panel's pause switch.

The page bridge runs in a WebKit isolated content world, separate from page JavaScript. It reads DOM elements, including open shadow roots, and invokes DOM actions. Password values are redacted from snapshots; other visible page data is available to an authorized local agent. Field values are not retained in the activity log. Website content is untrusted input for agents and must not override their user's instructions.

## What's deliberately still a prototype

This is a working browser foundation, not feature parity with Arc or Safari. WebExtensions and browser-data import are available through Settings; compatibility and supported migration formats are documented in [Extensions and import](docs/EXTENSIONS-IMPORT.md). Login saving and user-selected autofill support standard top-level HTTPS forms, including same-origin email-first flows. Saving requires confirmation; credentials remain in the profile’s macOS Keychain and are offered only for the exact HTTPS origin. Use the key in the sidebar address bar to fill; forms are never submitted automatically. Cross-origin iframes, closed shadow roots, signup/password-change forms and unusual custom login controls are not covered. No per-space cookie isolation or automatic updates yet. Embedded frames are listed but their content is not yet controlled; closed shadow roots and canvas-only interfaces are not covered. Some websites require trusted input events or reject embedded browsers. There is no visual fallback. Website alert/confirm dialogs may require the user to respond. Popups open a new tab, but complex popup-based sign-in needs further testing.

The **built-in Space assistant** opens on the right with ⇧⌘A. Configure a full OpenAI-compatible Chat Completions URL and a tool-capable model in the panel. HTTPS is required except for loopback servers. API keys are kept only for the current profile session, never on disk, and endpoint edits clear the key. The wire format follows the [Chat Completions reference](https://developers.openai.com/api/reference/resources/chat).

Each profile stores its own `space-chat.json` and `chat-connection.json`. Space renaming moves its timeline, notes and unsent context. **Work in Space** on a loaded email stages summary, link-opening, price-check or reply-drafting prompts; nothing is transmitted until Send. Sending includes the current Space's recent conversation, tab titles/URLs and selected email text. On-demand page reads go to the configured provider too. The panel shows this disclosure and lets users remove staged email context. History includes mail text and is stored locally; retention controls and encryption beyond macOS protections are not yet included.

YoBro does not bundle a local AI model. When a cloud provider such as OpenRouter or OpenAI is configured, chat plus every email, page, or note content retrieved by the agent is sent to that provider for processing. A localhost endpoint keeps YoBro's requests on the Mac, although YoBro cannot guarantee what separately installed local-server software does with them. The agent panel displays the active data path before use.

The assistant can read pages from its Space in separate agent tabs and request approval to open HTTP(S) links in the right agent pane. It has no click, form-fill, send, purchase, upload, delete or login-switch tools. Approvals show the full destination, can be declined, and expire on pause or Space/profile switch. Switching also cancels in-flight model work. Responses can be saved in the Space's Notes tab or copied as reply drafts. There is no automatic SMTP dispatch, persistent mail-draft object, task-list object or per-Space mailbox binding yet. Calls are bounded; streaming and native provider-specific APIs are not included. A model supporting the compatible tool-call format must be supplied; YoBro bundles no model or subscription.

External agents continue to use the local CLI/socket and retain their own approval policies. The built-in chat's restricted tools and approval UI do not wrap or restrict external agents beyond the existing local-access switch.

The implementation is original; no source from AgenticBrowser has been copied. That project's separate Electron and headless Chrome architecture was unsuitable for the WebKit requirement.

## Development and verification

```sh
swift build
YOBRO_HOME="$PWD/.runtime" YOBRO_DEV_CAPTURE=1 .build/debug/YOBRO
YOBRO_HOME="$PWD/.runtime" python3 tests/integration.py
YOBRO_HOME="$PWD/.runtime" python3 tests/daily_browser.py
```

The integration test uses a temporary localhost fixture. It checks navigation, DOM reads, form entry/submission, shadow DOM, stale-reference rejection, tabs, and shared cookies. Run in an isolated YoBro session (`YOBRO_HOME`) because it opens and manipulates test tabs. `YOBRO_DEV_CAPTURE=1` enables a development-only `capture-window` command that writes `window.png` into the runtime directory. Normal app launches do not expose capture.
