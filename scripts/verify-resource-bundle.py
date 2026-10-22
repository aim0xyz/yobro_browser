#!/usr/bin/env python3
"""Verify the resource path used by SwiftPM's generated Bundle.module accessor."""
from pathlib import Path
import sys


def verify(app: Path) -> None:
    bundle = app / "Contents/Resources/YOBRO_YOBRO.bundle"
    required = ("English.json", "AgentBridge.js", "MailWorker.py")
    missing = [name for name in required if not (bundle / name).is_file()]
    if missing:
        raise SystemExit(f"SwiftPM resource bundle missing at {bundle}: {', '.join(missing)}")


if __name__ == "__main__":
    verify(Path(sys.argv[1]))
