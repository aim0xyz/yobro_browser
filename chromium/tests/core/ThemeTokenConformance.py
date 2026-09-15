#!/usr/bin/env python3
"""Token conformance between the WebKit build's Theme.swift and the Qt build's Theme.cpp.

Both applications must show one visual identity. The WebKit source tokens are
the reference; this test fails when the Chromium palette drifts away from them,
or when a spike file hardcodes a hex colour outside the theme layer.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SWIFT_THEME = REPO / "Sources" / "YOBRO" / "Theme.swift"
CPP_THEME = REPO / "chromium" / "spike" / "Theme.cpp"
SPIKE_DIR = REPO / "chromium" / "spike"

# The start page and private-browsing page are rendered HTML content, not
# chrome; their restyle is its own milestone and they carry their own palette.
HEX_BAN_ALLOWLIST = {"SpikeWindowSupport.cpp"}

failures = []


def swift_tokens() -> dict[str, tuple[str, str]]:
    text = SWIFT_THEME.read_text()
    pattern = re.compile(
        r'static\s+let\s+(\w+)\s*=\s*adaptive\([^)]*?light:\s*0x([0-9A-Fa-f]{6})\s*,\s*dark:\s*0x([0-9A-Fa-f]{6})',
        re.S,
    )
    return {name: (light.upper(), dark.upper()) for name, light, dark in pattern.findall(text)}


def cpp_tokens(block: str) -> dict[str, str]:
    pattern = re.compile(r'/\*(\w+)\*/\s*"#([0-9A-Fa-f]{6})"')
    return {name: value.upper() for name, value in pattern.findall(block)}


def main() -> int:
    swift = swift_tokens()
    if not swift:
        failures.append("No adaptive tokens found in Sources/YOBRO/Theme.swift")

    cpp_text = CPP_THEME.read_text()
    light_match = re.search(r"constexpr\s+WebKitTokens\s+lightTokens\s*\{(.*?)\};", cpp_text, re.S)
    dark_match = re.search(r"constexpr\s+WebKitTokens\s+darkTokens\s*\{(.*?)\};", cpp_text, re.S)
    if not light_match or not dark_match:
        failures.append("Theme.cpp no longer exposes lightTokens/darkTokens with the /*name*/ comments")
    else:
        light = cpp_tokens(light_match.group(1))
        dark = cpp_tokens(dark_match.group(1))
        for name, (light_hex, dark_hex) in sorted(swift.items()):
            if light.get(name) != light_hex:
                failures.append(
                    f"light {name}: Theme.swift #{light_hex} but Theme.cpp #{light.get(name, 'MISSING')}"
                )
            if dark.get(name) != dark_hex:
                failures.append(
                    f"dark {name}: Theme.swift #{dark_hex} but Theme.cpp #{dark.get(name, 'MISSING')}"
                )
        extra = set(light) - set(swift)
        if extra:
            failures.append(f"Theme.cpp defines tokens Theme.swift does not have: {sorted(extra)}")

    pattern = re.compile(r"#[0-9a-fA-F]{3,8}\b")
    for source in sorted(SPIKE_DIR.glob("*.cpp")):
        if source.name == "Theme.cpp" or source.name in HEX_BAN_ALLOWLIST:
            continue
        for number, line in enumerate(source.read_text().splitlines(), start=1):
            stripped = line.split("//", 1)[0]
            match = pattern.search(stripped)
            if match:
                failures.append(
                    f"{source.name}:{number} hardcodes {match.group(0)} — use the Theme palette instead"
                )

    if failures:
        print("chromium-theme-tokens FAILED:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(f"chromium-theme-tokens PASS: {len(swift)} tokens in step with Theme.swift, spike files hex-free.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
