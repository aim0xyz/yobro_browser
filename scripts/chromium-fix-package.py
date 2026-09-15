#!/usr/bin/env python3
"""Remove non-portable absolute rpaths from a deployed macOS app and re-sign it."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys

MACHO_MAGICS = {
    b"\xce\xfa\xed\xfe",
    b"\xcf\xfa\xed\xfe",
    b"\xfe\xed\xfa\xce",
    b"\xfe\xed\xfa\xcf",
    b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca",
    b"\xca\xfe\xba\xbf",
    b"\xbf\xba\xfe\xca",
}
ALLOWED_ABSOLUTE_PREFIXES = ("/System/", "/usr/lib/")


def is_macho(path: Path) -> bool:
    if not path.is_file():
        return False
    try:
        with path.open("rb") as stream:
            return stream.read(4) in MACHO_MAGICS
    except OSError:
        return False


def absolute_rpaths(path: Path) -> set[str]:
    output = subprocess.run(
        ["/usr/bin/otool", "-l", str(path)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()
    result: set[str] = set()
    for index, line in enumerate(output):
        if line.strip() != "cmd LC_RPATH":
            continue
        for candidate in output[index + 1 : index + 6]:
            stripped = candidate.strip()
            if not stripped.startswith("path "):
                continue
            rpath = stripped.split()[1]
            if rpath.startswith("/") and not rpath.startswith(ALLOWED_ABSOLUTE_PREFIXES):
                result.add(rpath)
            break
    return result


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: chromium-fix-package.py <app-bundle>", file=sys.stderr)
        return 2
    app = Path(sys.argv[1]).resolve()
    contents = app / "Contents"
    if not contents.is_dir():
        print(f"App bundle is missing: {app}", file=sys.stderr)
        return 2

    changed = 0
    for path in contents.rglob("*"):
        if path.is_symlink() or not is_macho(path):
            continue
        for rpath in absolute_rpaths(path):
            subprocess.run(
                ["/usr/bin/install_name_tool", "-delete_rpath", rpath, str(path)],
                check=True,
            )
            changed += 1

    subprocess.run(
        ["/usr/bin/codesign", "--force", "--deep", "--sign", "-", str(app)],
        check=True,
    )
    print(f"CHROMIUM PACKAGE FIXUP: removed {changed} external rpaths and re-signed ad hoc")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
