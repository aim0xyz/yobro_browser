#!/usr/bin/env python3
"""Check that the bundled uBlock Origin Lite keeps up with upstream.

Online (the default) compares the bundled release with the latest uBOL-home
release and with the minimum named in uAssets#30158, YouTube's tracking issue.
--offline only checks the release date encoded in the tag.

Exit status: 0 current; 1 behind the latest release, older than --max-age-days,
or the status could not be determined; 2 below the YouTube minimum.
"""
import argparse
import datetime
import importlib.util
import json
from pathlib import Path
import re
import sys

_spec = importlib.util.spec_from_file_location("prepare_bundled_ublock", Path(__file__).with_name("prepare-bundled-ublock.py"))
prepare = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(prepare)

LATEST_RELEASE = "https://api.github.com/repos/uBlockOrigin/uBOL-home/releases/latest"
YOUTUBE_ISSUE = "https://api.github.com/repos/uBlockOrigin/uAssets/issues/30158"
YOUTUBE_ISSUE_PAGE = "https://github.com/uBlockOrigin/uAssets/issues/30158"
CURRENT, BEHIND, BELOW_MINIMUM = 0, 1, 2


def version_key(version):
    return tuple(int(part) for part in version.split("."))


def bundled_tag(package):
    """The upstream tag, without YoBro's compatibility patch revision."""
    return ".".join(prepare.manifest(package.read_bytes())["version"].split(".")[:3])


def tag_age_days(tag, today):
    return (today - datetime.date(*prepare.tag_date(tag))).days


def minimum_from_issue(body):
    """The issue's checklist asks for "the latest versions (`2026.1018.2229`+)"."""
    versions = re.findall(r"`(\d{4}\.\d{3,4}\.\d{3,4})`\s*\+", body or "")
    return max(versions, key=version_key, default=None)


def evaluate(bundled, latest, minimum):
    """Return the exit status and the lines explaining it."""
    lines = []
    status = CURRENT
    if minimum is None:
        lines.append(f"Could not read the YouTube minimum; check {YOUTUBE_ISSUE_PAGE} by hand.")
        status = BEHIND
    elif version_key(bundled) < version_key(minimum):
        lines.append(f"Below the YouTube minimum {minimum}: YouTube playback is likely broken. Update before releasing.")
        status = BELOW_MINIMUM
    if version_key(bundled) < version_key(latest):
        lines.append(f"Behind the latest upstream release {latest}.")
        status = max(status, BEHIND)
    if status == CURRENT:
        lines.append("Current.")
    return status, lines


def fetch_json(url):
    return json.loads(prepare.fetch(url, "Accept: application/vnd.github+json"))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--bundle", type=Path, default=prepare.RESOURCES / "uBlockOriginLite.safari.zip")
    parser.add_argument("--offline", action="store_true", help="Only check the age of the bundled release")
    parser.add_argument("--max-age-days", type=int, default=14)
    args = parser.parse_args(argv)
    bundled = bundled_tag(args.bundle)
    age = tag_age_days(bundled, datetime.date.today())

    if args.offline:
        if age > args.max_age_days:
            print(f"warning: bundled uBlock Origin Lite {bundled} is {age} days old. "
                  "YouTube breaks old releases; run scripts/check-ublock-freshness.py.", file=sys.stderr)
            return BEHIND
        return CURRENT

    print(f"Bundled uBlock Origin Lite: {bundled} ({age} days old)")
    try:
        release = fetch_json(LATEST_RELEASE)
        issue = fetch_json(YOUTUBE_ISSUE)
    except (OSError, ValueError) as error:
        print(f"Could not read the upstream status from GitHub: {error}", file=sys.stderr)
        return BEHIND
    latest = release["tag_name"]
    minimum = minimum_from_issue(issue.get("body"))
    print(f"Latest upstream release:   {latest}")
    print(f"YouTube minimum (uAssets#30158): {minimum or 'unknown'}")
    status, lines = evaluate(bundled, latest, minimum)
    print("\n".join(lines))
    if version_key(bundled) < version_key(latest):
        asset = next((a for a in release.get("assets", []) if a["name"] == f"uBOLite_{latest}.safari.zip"), {})
        digest = asset.get("digest", "").removeprefix("sha256:") or "<SHA-256 from the release page>"
        print(f"Review {prepare.RELEASES}/tag/{latest}, then run:\n"
              f"  python3 scripts/prepare-bundled-ublock.py --tag {latest} --sha256 {digest}")
    return status


if __name__ == "__main__":
    sys.exit(main())
