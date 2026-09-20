#!/usr/bin/env python3
"""Captures the QML shell's start page in light and dark and compares it
against the WebKit reference captures.

Each run gets an isolated Chromium home so the capture state is deterministic:
a fresh default profile, one start-page tab, English UI, fixed window format.
The comparison itself lives in VisualReferenceDiff.py and acts as a warning
system with a hard structural floor.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

DIFF_SCRIPT = Path(__file__).resolve().parent / "VisualReferenceDiff.py"

HARD_SCORE = 45.0


def capture(binary: Path, appearance: str, out_path: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="yobro-qml-capture-") as home:
        env = dict(os.environ)
        env["YOBRO_CHROMIUM_HOME"] = home
        env["YOBRO_TEST_APPEARANCE"] = appearance
        result = subprocess.run(
            [str(binary), "--capture", str(out_path)],
            env=env,
            capture_output=True,
            text=True,
            timeout=240,
        )
    if result.returncode != 0:
        print(f"capture ({appearance}) FAILED with exit {result.returncode}")
        print(result.stdout)
        print(result.stderr)
        raise SystemExit(1)
    if not out_path.exists():
        print(f"capture ({appearance}) FAILED: no PNG written")
        raise SystemExit(1)


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: QmlShellCapture.py <qml-binary> <golden-dir> <output-dir>")
        return 1
    binary = Path(sys.argv[1])
    golden = Path(sys.argv[2])
    out_dir = Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)

    failures = []
    for appearance in ("light", "dark"):
        out_path = out_dir / f"startpage-{appearance}.png"
        capture(binary, appearance, out_path)
        reference = golden / f"ref-webkit-{appearance}.png"
        diff_path = out_dir / f"startpage-{appearance}-diff.png"
        diff = subprocess.run(
            [sys.executable, str(DIFF_SCRIPT), str(reference), str(out_path), str(diff_path), str(HARD_SCORE)],
            capture_output=True,
            text=True,
        )
        print(diff.stdout.rstrip())
        if diff.returncode != 0:
            failures.append(appearance)
            if diff.stderr.strip():
                print(diff.stderr.rstrip())

    if failures:
        print(f"chromium-qml-shell-capture FAILED for: {', '.join(failures)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
