#!/usr/bin/env python3
"""Check bundled dynamic frameworks before distributing a YoBro app."""
from pathlib import Path
import subprocess
import sys


def output(*args: str) -> str:
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout


def verify(app: Path) -> None:
    executable = app / "Contents/MacOS/YOBRO"
    libraries = output("/usr/bin/otool", "-L", str(executable))
    if "@rpath/Sparkle.framework/Versions/B/Sparkle" not in libraries:
        return
    framework = app / "Contents/Frameworks/Sparkle.framework/Versions/B/Sparkle"
    if not framework.is_file():
        raise SystemExit("Sparkle is linked but its binary is missing from the app bundle")
    load_commands = output("/usr/bin/otool", "-l", str(executable))
    if "path @executable_path/../Frameworks (offset" not in load_commands:
        raise SystemExit("Sparkle is linked but the bundled Frameworks runpath is missing")


if __name__ == "__main__":
    verify(Path(sys.argv[1]))
