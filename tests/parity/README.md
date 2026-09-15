# Parity tests — shared black-box journeys for WebKit + Chromium

Per migration plan: the same suite runs against both applications. Every README
feature has an owner, automated coverage where practical, and documented manual
verification otherwise.

- `parity-matrix.md` — the tracking matrix (states: not started | implemented |
  verified on one platform | verified on all platforms).
- `parity_runner.py` — Phase-2 contract gate: validates `contracts/` fixtures
  without launching a web page (JSON parse, command list vs `ControlBridge`,
  schema-required keys, socket/IPC rules). Web-page journeys (navigation, DOM,
  shadow DOM, stale refs, cookies) run via `tests/integration.py`-style
  harnesses against each engine in Phase 4/5.
- `fixtures/` — golden screenshots + large-profile fixtures land here in Phase 0
  (light/dark, agreed window sizes, DE/EN). Tolerances are explicitly reviewed.
