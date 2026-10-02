#!/usr/bin/env python3
"""Fail packaging when the signed app cannot request microphone or Apple Events access."""
import plistlib
import subprocess
import sys
from pathlib import Path


def verify(app: Path) -> None:
    result = subprocess.run(["/usr/bin/codesign", "-d", "--entitlements", ":-", str(app)],
                            check=True, capture_output=True)
    xml = result.stdout.find(b"<?xml")
    if xml < 0:
        raise SystemExit("YoBro is signed without entitlements")
    entitlements = plistlib.loads(result.stdout[xml:])
    if entitlements.get("com.apple.security.device.audio-input") is not True:
        raise SystemExit("YoBro is signed without the Audio Input entitlement")
    if entitlements.get("com.apple.security.automation.apple-events") is not True:
        raise SystemExit("YoBro is signed without the Apple Events entitlement")


if __name__ == "__main__":
    verify(Path(sys.argv[1]))
