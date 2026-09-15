# Phase 0 — WebKit reference freeze (per migration plan)

Reproducible fallback build; source revision archived at migration start.

## Repro

```sh
./scripts/build.sh
open dist/YoBro.app
```

- Requires macOS 14+, Xcode CLI tools, Python 3 (CLI only).
- Output: `dist/YoBro.app` (+ `Contents/MacOS/yobroctl`). No third-party packages.
- Archive: record `git rev-parse HEAD` (or source snapshot hash) + signed build
  artifact before starting Phase 3 production UI. ← TODO: hash eintragen

## Baselines to capture (exit gate)

- [ ] Clean-build procedure verified by a second developer without real profile.
- [ ] Golden screenshots: every primary screen × light/dark × expanded/compact ×
      split × empty/loading/error × DE/EN → `tests/parity/fixtures/` (TODO).
- [ ] Parity matrix seeded from README + test suites → `tests/parity/parity-matrix.md` (done, initial).
- [ ] Sanitized fixtures per persisted file + realistic large profile (TODO).
- [ ] Perf baseline on Intel + Apple Silicon: cold start, idle RAM, 10-tab RAM,
      page-load CPU, energy, app size (TODO — record identical hardware + pages).

Contract gate (no web page): `python3 tests/parity/parity_runner.py` — PASS.
Web-page gate: `YOBRO_HOME="$PWD/.runtime" python3 tests/integration.py` (isolated session).
