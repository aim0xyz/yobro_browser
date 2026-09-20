# Agent control audit — 2026-09-17

The running WebKit browser was tested through the user-specified local socket at
`~/Library/Application Support/YOBRO/control.sock`. All interaction used temporary
agent tabs and a localhost fixture. The corrected debug build was subsequently
started with a temporary `YOBRO_HOME` and retested through its own socket. Both
sessions were ended and their test tabs closed. The running installation was not
replaced or restarted.

## Verified capabilities

- Open and read pages, including element references and document IDs.
- Fill text inputs and select options, click controls, and read checkbox state.
- Redact password values in page snapshots.
- Dispatch keyboard events through the new MCP `press_key` tool, including form
  submission with Enter. This uses DOM events, not native OS keystrokes. Other
  keys reach page handlers but do not implement native typing or Tab traversal.
- Scroll and report viewport size, scroll position, and document dimensions.
- Find text, navigate back/forward, reload, duplicate and close agent tabs.
- End the agent session and remove temporary agent tabs.

History and download listing were reported as disabled by the profile's library
access setting. That setting was not changed. Download side effects and external
account actions were outside this fixture audit. Frame contents, file uploads,
and screenshots are not exposed by the current MCP tool set.

## Fixes

1. Restore document-ID validation before resolving or acting on an element ref.
2. Fill native inputs with `role="textbox"` via their value setter, not by treating
   them as contenteditable containers.
3. Reject disabled controls, including disabled fieldsets, `aria-disabled`
   ancestors, and inert ancestors; report their disabled state in snapshots.
4. Reject missing, ambiguous, or disabled select options without clearing the
   existing selection. Prefer exact, case-sensitive option values.
5. Reject checkbox/button/file inputs in `fill` and respect cancelled
   `beforeinput` before changing a native field.
6. Respect cancelled Enter events and avoid submitting textarea forms.
7. Validate MCP required arguments, types, enum values, bounds, and unexpected
   properties before sending commands. In particular, `navigate` cannot be used
   to dispatch arbitrary commands.
8. Distinguish socket permission errors from a stopped browser and correct the
   session-ending tool description to match the current browser behavior.

## Verification

- Five `AgentBridgeTests` passed against real WebKit.
- Three MCP validation tests passed, including eight invalid-call cases.
- The MCP stdio/socket smoke test passed.
- `tests/agent_control_audit.py` passed against the corrected isolated build.

To repeat the live audit, explicitly set `YOBRO_SOCKET` to a test instance with
Agent Access enabled and no existing agent session, then run
`python3 tests/agent_control_audit.py`. It serves its own localhost fixture and
cleans up its agent tabs. On macOS, WebKit tests and local sockets may require
execution outside a restricted tool sandbox.
