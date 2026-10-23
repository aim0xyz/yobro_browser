#!/usr/bin/env python3
"""Bundle an official uBlock Origin Lite Safari release with YoBro's reviewed WebKit patch.

    scripts/prepare-bundled-ublock.py --tag 2026.1021.1354 --sha256 <digest>

The digest is the Safari asset's SHA-256 on the uBOL-home release page;
scripts/check-ublock-freshness.py prints the full command for the latest
release. Without --source the asset is downloaded from that release. Writes
the patched package and its notice into Sources/YOBRO/Resources.

uBOL's filter lists are compiled into each release and never update on their
own. YouTube breaks old releases within weeks, so follow upstream closely:
https://github.com/uBlockOrigin/uAssets/issues/30158 names the minimum.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent.parent
RESOURCES = ROOT / "Sources/YOBRO/Resources"
RELEASES = "https://github.com/uBlockOrigin/uBOL-home/releases"
# YoBro compatibility patch revision, appended to the upstream version.
PATCH = 1
ENGINE_CHECK = b"if ( extURL.startsWith('safari-web-extension:') ) { return 'safari'; }"
ENGINE_PATCH = b"// YoBro: WKWebExtensionController uses webkit-extension, with Safari APIs.\n    if ( /^(?:safari-web-extension|webkit-extension):/.test(extURL) ) { return 'safari'; }"
# Upgrades keep the permissions granted to the previous version, so a newly
# requested permission would silently stay ungranted.
PERMISSION_KEYS = ("permissions", "optional_permissions", "host_permissions", "optional_host_permissions")


def tag_date(tag):
    """uBOL tags are year.monthDD.hourMM, for example 2026.914.1325."""
    if not re.fullmatch(r"\d{4}\.\d{3,4}\.\d{3,4}", tag):
        raise ValueError(f"Not a uBOL release tag: {tag}")
    year, day = tag.split(".")[:2]
    return int(year), int(day[:-2]), int(day[-2:])


def asset_url(tag):
    return f"{RELEASES}/download/{tag}/uBOLite_{tag}.safari.zip"


def fetch(url, *headers):
    """curl uses the macOS trust store; python.org builds may lack CA certificates."""
    command = ["/usr/bin/curl", "--fail", "--silent", "--show-error", "--location", "--max-time", "120"]
    for header in headers:
        command += ["--header", header]
    result = subprocess.run(command + [url], capture_output=True)
    if result.returncode != 0:
        raise OSError(result.stderr.decode(errors="replace").strip() or f"curl failed for {url}")
    return result.stdout


def manifest(package):
    with zipfile.ZipFile(io.BytesIO(package)) as archive:
        return json.loads(archive.read("manifest.json"))


def patch(original, tag):
    """Apply the engine-detection patch; everything else stays byte-identical."""
    output = io.BytesIO()
    with zipfile.ZipFile(io.BytesIO(original)) as archive, zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as target:
        for info in archive.infolist():
            data = archive.read(info.filename)
            if info.filename == "js/ext.js":
                if data.count(ENGINE_CHECK) != 1:
                    raise SystemExit("Engine detection changed in js/ext.js; manual review required")
                data = data.replace(ENGINE_CHECK, ENGINE_PATCH)
            elif info.filename == "manifest.json":
                upstream = json.loads(data)
                if upstream["version"] != tag:
                    raise SystemExit(f"Manifest version {upstream['version']} does not match tag {tag}")
                upstream["version"] = f"{tag}.{PATCH}"
                upstream["version_name"] = f"{tag} + YoBro WebKit compatibility {PATCH}"
                data = (json.dumps(upstream, indent=2) + "\n").encode()
            target.writestr(info, data)
        notice = zipfile.ZipInfo("YOBRO-COMPATIBILITY.txt", (*tag_date(tag), 0, 0, 0))
        target.writestr(notice, f"YoBro compatibility patch {PATCH}: recognize webkit-extension: as Safari in js/ext.js. Manifest version incremented to reinitialize cached filtering rules. All filtering code and lists otherwise unchanged. Based on official uBlock Origin Lite Safari {tag}. GPLv3; source: https://github.com/gorhill/uBlock/tree/master/platform/mv3\n")
    return output.getvalue()


def permission_changes(previous, current):
    changes = []
    for key in PERMISSION_KEYS:
        old, new = set(previous.get(key, [])), set(current.get(key, []))
        changes += [f"+ {key}: {value}" for value in sorted(new - old)]
        changes += [f"- {key}: {value}" for value in sorted(old - new)]
    return changes


def notice(tag, upstream_sha, bundled_sha):
    return f"""uBlock Origin Lite, Raymond Hill and contributors
Based on official Safari release: {tag}
YoBro compatibility version: {tag}.{PATCH}
{RELEASES}/tag/{tag}
Source: https://github.com/gorhill/uBlock/tree/master/platform/mv3
License: GNU GPL version 3; complete license and upstream README are inside uBlockOriginLite.safari.zip.
The Safari package requires Safari/WebKit 18.6 (macOS 15.6) or later.
YoBro modification: js/ext.js recognizes webkit-extension: as Safari, not Chromium.
Manifest version incremented to reinitialize filtering rules. Filter lists are unchanged.
Reproducible patch: scripts/prepare-bundled-ublock.py (repository root).
Upstream SHA-256: {upstream_sha}
Bundled SHA-256: {bundled_sha}
"""


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tag", required=True, help="uBOL-home release tag, for example 2026.1021.1354")
    parser.add_argument("--sha256", required=True, help="SHA-256 of the official Safari zip of that release")
    parser.add_argument("--source", type=Path, help="Already downloaded official Safari zip")
    parser.add_argument("--output", type=Path, default=RESOURCES / "uBlockOriginLite.safari.zip")
    parser.add_argument("--notice", type=Path, default=RESOURCES / "uBlockOriginLite-NOTICE.txt")
    parser.add_argument("--accept-permission-changes", action="store_true",
                        help="Bundle a release that requests different permissions than the current package")
    args = parser.parse_args(argv)
    try:
        tag_date(args.tag)
    except ValueError as error:
        parser.error(str(error))
    expected = args.sha256.lower().removeprefix("sha256:")
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        parser.error("--sha256 must be 64 hexadecimal digits")

    try:
        original = args.source.read_bytes() if args.source else fetch(asset_url(args.tag))
    except OSError as error:
        raise SystemExit(f"Could not read the upstream archive: {error}")
    upstream_sha = hashlib.sha256(original).hexdigest()
    if upstream_sha != expected:
        raise SystemExit(f"Unexpected upstream archive (SHA-256 {upstream_sha}); review the release before patching")
    bundled = patch(original, args.tag)
    if args.output.exists():
        changes = permission_changes(manifest(args.output.read_bytes()), manifest(bundled))
        if changes:
            print("Requested permissions changed:\n  " + "\n  ".join(changes), file=sys.stderr)
            if not args.accept_permission_changes:
                raise SystemExit("Existing installs keep their previous permissions on upgrade. Review the change, "
                                 "then rerun with --accept-permission-changes.")
    bundled_sha = hashlib.sha256(bundled).hexdigest()
    args.output.write_bytes(bundled)
    args.notice.write_text(notice(args.tag, upstream_sha, bundled_sha))
    print(f"Bundled uBlock Origin Lite {args.tag}.{PATCH}: {bundled_sha}")


if __name__ == "__main__":
    main()
