#!/usr/bin/env python3
"""Proves the QML shell's address navigation end to end.

The binary is started with a file:// URL; the shell must route it through the
browser model (openAddress → navigationRequested → WebEngineView) and show the
loaded page instead of the start page. The page paints a single deterministic
colour, so the assertion is a pixel check in the content area — the start page
never paints it.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

PAGE_COLOUR = (43, 98, 62)  # the page body background


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: QmlShellNavigation.py <qml-binary> <output-dir>")
        return 1
    binary = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="yobro-qml-nav-") as home:
        page = Path(home) / "page.html"
        page.write_text(
            "<!doctype html><html><head><title>Orbit Navigation Probe</title></head>"
            f"<body style=\"margin:0;background:rgb{PAGE_COLOUR};\">"
            "</body></html>",
            encoding="utf-8",
        )
        out_path = out_dir / "navigation.png"
        env = dict(os.environ)
        env["YOBRO_CHROMIUM_HOME"] = home
        env["YOBRO_LANGUAGE"] = "en"
        result = subprocess.run(
            [str(binary), "--capture", str(out_path), "--url", page.as_uri()],
            env=env,
            capture_output=True,
            text=True,
            timeout=240,
        )
        if result.returncode != 0:
            print("chromium-qml-shell-navigation FAILED: capture exited "
                  f"{result.returncode}")
            print(result.stdout)
            print(result.stderr)
            return 1

    if not out_path.exists():
        print("chromium-qml-shell-navigation FAILED: no PNG written")
        return 1

    image = Image.open(out_path).convert("RGB")
    # The content card starts right of the 248pt sidebar; probe its middle.
    centre_x = (248 + 10 + image.width / 2) // 2  # logical → device (2x)
    centre_y = image.height // 2
    sample = image.getpixel((int(centre_x), int(centre_y)))
    if all(abs(sample[i] - PAGE_COLOUR[i]) <= 6 for i in range(3)):
        print(f"chromium-qml-shell-navigation OK: web page painted {sample}")
        return 0
    print(
        "chromium-qml-shell-navigation FAILED: expected the page colour "
        f"~{PAGE_COLOUR} in the content area, found {sample}"
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
