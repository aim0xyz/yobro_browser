#!/usr/bin/env python3
"""Fail a release on recognizable secrets/private state. Never print matched values.

This is a packaging guard, not a proof that arbitrary binaries contain no secrets.
No Keychain, live profile, credential store, or developer environment is read.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import stat
import zipfile

RULES = {
    "private-key": rb"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----",
    "provider-secret": rb"(?<![A-Za-z0-9_-])(?:sk-(?:proj-|or-v1-|ant-api\d+-)[A-Za-z0-9_-]{25,}|sk-[A-Za-z0-9]{32,}|AKIA[A-Z0-9]{16}|gh[pousr]_[A-Za-z0-9]{30,}|sb_secret_[A-Za-z0-9_-]{20,})",
    "vpn-access-key": rb"ss://[A-Za-z0-9+/_=-]{12,}",
    "credential-url": rb"[a-zA-Z][a-zA-Z0-9+.-]*://[^\s/\x00:@]+:[^\s/\x00@]+@",
    "personal-build-path": rb"/Users/[A-Za-z0-9_.-]+/",
    "jwt-secret": rb"eyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{16,}",
}
PRIVATE_NAMES = {"cookies", "cookies.sqlite", "login data", "history", "history.db", "history.sqlite",
                 "profiles.json", "session.json", "onboarding.json", "managed-vpn.json", "managed-vpn-locations.json",
                 "agent-access.json", "bridge-policy.json", "supabase.json", "space-chat.json", "mail-accounts.json",
                 ".yobro-signing-identity", ".env", "auth.json", "config.toml"}

# Sparkle is a versioned macOS framework. Its normal framework entry points
# are relative symlinks into Versions/Current; all other package symlinks are
# still rejected below.
ALLOWED_FRAMEWORK_SYMLINK_PREFIX = "Contents/Frameworks/Sparkle.framework/"


def inspect_bytes(name, data, failures, depth=0):
    for rule, pattern in RULES.items():
        if re.search(pattern, data):
            failures.append({"file": name, "rule": rule})
    if data.startswith(b"PK\x03\x04"):
        if depth >= 4:
            failures.append({"file": name, "rule": "archive-depth"}); return
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                total = 0
                for info in archive.infolist():
                    total += info.file_size
                    nested = name + "!/" + info.filename
                    if total > 512 * 1024 * 1024:
                        failures.append({"file": name, "rule": "archive-size"}); break
                    path = Path(info.filename)
                    if path.is_absolute() or ".." in path.parts or stat.S_ISLNK(info.external_attr >> 16):
                        failures.append({"file": nested, "rule": "unsafe-archive-path"}); continue
                    inspect_name(nested, failures)
                    if not info.is_dir(): inspect_bytes(nested, archive.read(info), failures, depth + 1)
        except (zipfile.BadZipFile, RuntimeError):
            failures.append({"file": name, "rule": "invalid-archive"})


def inspect_name(name, failures):
    parts = [part.lower() for part in Path(name).parts]
    if any(part in PRIVATE_NAMES or part.startswith(".env.") or part.endswith((".keychain", ".keychain-db", ".p12", ".pem")) for part in parts):
        failures.append({"file": name, "rule": "private-state-file"})


def audit(root):
    failures, inventory = [], []
    for file in sorted(root.rglob("*")):
        name = str(file.relative_to(root))
        if file.is_symlink():
            if not name.startswith(ALLOWED_FRAMEWORK_SYMLINK_PREFIX):
                failures.append({"file": name, "rule": "unexpected-symlink"})
            continue
        if not file.is_file(): continue
        inspect_name(name, failures)
        data = file.read_bytes()
        inspect_bytes(name, data, failures)
        inventory.append({"file": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    return {"passed": not failures, "scope": "Packaged files and nested ZIP/MCPB contents; pattern-based, no personal stores accessed",
            "failures": failures, "files": inventory}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("app", type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    result = audit(args.app)
    if args.report: args.report.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"passed": result["passed"], "files_checked": len(result["files"]), "findings": result["failures"]}, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
