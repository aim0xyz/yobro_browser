#!/usr/bin/env python3
"""Validate the engine-neutral contracts without launching a web page.

Checks:
1. Agent protocol command list matches the frozen set from ControlBridge.swift.
2. Protocol status/version and exact library/download values agree across
   fixtures and both browser implementations.
3. Every contract JSON file parses.
4. Persisted library and session fixtures satisfy required schema shapes using
   only the Python standard library.
5. The existing WebKit source-of-truth paths remain present.
"""
import json
import math
import re
import sys
from datetime import datetime
from pathlib import Path
from uuid import UUID

ROOT = Path(__file__).resolve().parents[2]
CONTRACTS = ROOT / "contracts"
FAILURES: list[str] = []
JSON_CACHE: dict[Path, object | None] = {}

EXPECTED_COMMANDS = ["status", "tabs", "open", "new", "focus", "close", "read",
    "click", "fill", "scroll", "back", "forward", "reload", "pin", "split",
    "find", "history", "downloads", "download", "cancel-download",
    "duplicate", "end"]
DOWNLOAD_FIELDS = {
    "id", "name", "source", "date", "state", "received", "expected", "path", "error"
}
DOWNLOAD_REQUIRED_FIELDS = DOWNLOAD_FIELDS - {"path", "error"}
DOWNLOAD_STATES = {"downloading", "completed", "cancelled", "failed", "interrupted"}
DOWNLOAD_NEXT = "Use downloads to check progress and retrieve the final file path."
DOWNLOAD_ERRORS = {
    "invalidDownloadURL": "Gültige HTTP- oder HTTPS-Downloadadresse erforderlich.",
    "unknownDownload": "Unbekannter Download.",
    "inactiveDownload": "Download ist nicht aktiv.",
}


def load_json(path: Path):
    if path in JSON_CACHE:
        return JSON_CACHE[path]
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        FAILURES.append(f"{path.relative_to(ROOT)}: invalid JSON ({exc})")
        value = None
    JSON_CACHE[path] = value
    return value


def check_all_contract_json():
    for path in sorted(CONTRACTS.rglob("*.json")):
        load_json(path)


def check_commands():
    doc = load_json(CONTRACTS / "agent-protocol" / "v2-commands.json")
    if not isinstance(doc, dict):
        return
    if doc.get("commands") != EXPECTED_COMMANDS:
        FAILURES.append("agent-protocol/v2-commands.json: command list drifted from ControlBridge.swift")
    if doc.get("protocol") != 2:
        FAILURES.append("agent-protocol/v2-commands.json: protocol must stay 2 until a versioned bump")


def check_protocol_contract_values():
    command_contract = load_json(CONTRACTS / "agent-protocol" / "v2-commands.json")
    status_fixture = load_json(CONTRACTS / "agent-protocol" / "fixtures/status.json")
    error_fixture = load_json(CONTRACTS / "agent-protocol" / "fixtures/errors.json")
    if not all(isinstance(value, dict) for value in [command_contract, status_fixture, error_fixture]):
        return

    status = status_fixture.get("responseResult")
    if not isinstance(status, dict):
        FAILURES.append("agent-protocol/fixtures/status.json: responseResult must be an object")
        return
    if status.get("protocol") != command_contract.get("protocol"):
        FAILURES.append("agent-protocol/fixtures/status.json: protocol differs from v2-commands.json")
    if status.get("commands") != command_contract.get("commands"):
        FAILURES.append("agent-protocol/fixtures/status.json: commands differ from v2-commands.json")

    version = status.get("version")
    cmake_source = (ROOT / "chromium/CMakeLists.txt").read_text(encoding="utf-8")
    version_match = re.search(
        r"project\s*\(\s*YOBROChromium\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\b",
        cmake_source,
    )
    if not isinstance(version, str):
        FAILURES.append("agent-protocol/fixtures/status.json: version must be a string")
    elif version_match is None:
        FAILURES.append("chromium/CMakeLists.txt: could not determine project version")
    else:
        if version != version_match.group(1):
            FAILURES.append("agent-protocol/fixtures/status.json: version differs from Chromium project version")
        webkit_source = (ROOT / "Sources/YOBRO/ControlBridge.swift").read_text(encoding="utf-8")
        if f'"version": "{version}"' not in webkit_source:
            FAILURES.append("Sources/YOBRO/ControlBridge.swift: status version differs from fixture")

    library_gate = error_fixture.get("libraryGated")
    library_error = library_gate.get("error") if isinstance(library_gate, dict) else None
    if not isinstance(library_error, str):
        FAILURES.append("agent-protocol/fixtures/errors.json: libraryGated.error must be a string")
        library_error = None

    frozen_texts = [DOWNLOAD_NEXT, *DOWNLOAD_ERRORS.values()]
    if library_error is not None:
        frozen_texts.append(library_error)
    for source_path in [
        "Sources/YOBRO/ControlBridge.swift",
        "chromium/controller/src/BrowserSession.cpp",
    ]:
        source = (ROOT / source_path).read_text(encoding="utf-8")
        for text in frozen_texts:
            if text not in source:
                FAILURES.append(f"{source_path}: exact protocol text differs from fixture: {text}")


def schema_required(node: object, label: str) -> set[str]:
    if not isinstance(node, dict) or not isinstance(node.get("required"), list):
        FAILURES.append(f"{label}: required must be an array")
        return set()
    required = node["required"]
    if not all(isinstance(value, str) for value in required):
        FAILURES.append(f"{label}: required entries must be strings")
        return set()
    return set(required)


def check_library_contract():
    schema = load_json(CONTRACTS / "persisted-state" / "v1-library.schema.json")
    fixture = load_json(CONTRACTS / "persisted-state" / "fixtures" / "library-v1.json")
    if not isinstance(schema, dict) or not isinstance(fixture, dict):
        return

    properties = schema.get("properties")
    definitions = schema.get("$defs")
    if not isinstance(properties, dict) or not isinstance(definitions, dict):
        FAILURES.append("persisted-state/v1-library.schema.json: properties and $defs must be objects")
        return

    history_schema = properties.get("history.json")
    history_items = history_schema.get("items") if isinstance(history_schema, dict) else None
    history_properties = history_items.get("properties") if isinstance(history_items, dict) else None
    expected_history = {"id", "title", "url", "date", "visits"}
    if not isinstance(history_properties, dict) or set(history_properties) != expected_history:
        FAILURES.append("persisted-state/v1-library.schema.json: history entry shape drifted")
    if schema_required(history_items, "persisted-state/v1-library.schema.json history item") != expected_history:
        FAILURES.append("persisted-state/v1-library.schema.json: history required fields drifted")

    date_schema = definitions.get("persistedDate")
    date_variants = date_schema.get("oneOf") if isinstance(date_schema, dict) else None
    has_number = isinstance(date_variants, list) and any(
        isinstance(value, dict) and value.get("type") == "number" for value in date_variants
    )
    has_iso_string = isinstance(date_variants, list) and any(
        isinstance(value, dict) and value.get("type") == "string" and value.get("format") == "date-time"
        for value in date_variants
    )
    if not has_number or not has_iso_string:
        FAILURES.append("persisted-state/v1-library.schema.json: persistedDate must accept WebKit numbers and ISO date-time strings")

    download_schema = properties.get("downloads.json")
    download_items = download_schema.get("items") if isinstance(download_schema, dict) else None
    download_properties = download_items.get("properties") if isinstance(download_items, dict) else None
    if not isinstance(download_properties, dict) or set(download_properties) != DOWNLOAD_FIELDS:
        FAILURES.append("persisted-state/v1-library.schema.json: download entry shape drifted")
    if schema_required(download_items, "persisted-state/v1-library.schema.json download item") != DOWNLOAD_REQUIRED_FIELDS:
        FAILURES.append("persisted-state/v1-library.schema.json: download required fields drifted")

    state_schema = definitions.get("downloadState")
    state_enum = state_schema.get("enum") if isinstance(state_schema, dict) else None
    if not isinstance(state_enum, list) or set(state_enum) != DOWNLOAD_STATES:
        FAILURES.append("persisted-state/v1-library.schema.json: download state enum drifted")
    if isinstance(download_properties, dict):
        for field in ["path", "error"]:
            field_schema = download_properties.get(field)
            field_types = field_schema.get("type") if isinstance(field_schema, dict) else None
            if not isinstance(field_types, list) or set(field_types) != {"string", "null"}:
                FAILURES.append(f"persisted-state/v1-library.schema.json: {field} must be nullable")

    expected_files = {"history.json", "bookmarks.json", "downloads.json", "bridge-policy.json"}
    if set(fixture) != expected_files:
        FAILURES.append("persisted-state/fixtures/library-v1.json: fixture file set drifted")
        return
    history = fixture.get("history.json")
    if not isinstance(history, list) or not history:
        FAILURES.append("persisted-state/fixtures/library-v1.json: history.json must be a non-empty array")
    else:
        for index, entry in enumerate(history):
            label = f"persisted-state/fixtures/library-v1.json history[{index}]"
            if not isinstance(entry, dict) or not expected_history.issubset(entry):
                FAILURES.append(f"{label}: missing required fields")
                continue
            if not is_uuid(entry.get("id")):
                FAILURES.append(f"{label}: id must be a UUID")
            if not is_persisted_date(entry.get("date")):
                FAILURES.append(f"{label}: date must be an ISO date-time string or finite number")
            if not is_integer(entry.get("visits")):
                FAILURES.append(f"{label}: visits must be an integer")

    downloads = fixture.get("downloads.json")
    if not isinstance(downloads, list) or not downloads:
        FAILURES.append("persisted-state/fixtures/library-v1.json: downloads.json must be a non-empty array")
    else:
        for index, entry in enumerate(downloads):
            validate_download_entry(entry, f"persisted-state/fixtures/library-v1.json downloads[{index}]", wire_date=False)

    policy_schema = properties.get("bridge-policy.json")
    policy_required = schema_required(
        policy_schema,
        "persisted-state/v1-library.schema.json bridge-policy.json",
    )
    if policy_required != {"allowsLibraryAccess"}:
        FAILURES.append("persisted-state/v1-library.schema.json: bridge policy must require allowsLibraryAccess")
    policy_properties = policy_schema.get("properties") if isinstance(policy_schema, dict) else None
    allows_schema = policy_properties.get("allowsLibraryAccess") if isinstance(policy_properties, dict) else None
    if not isinstance(allows_schema, dict) or allows_schema.get("type") != "boolean":
        FAILURES.append("persisted-state/v1-library.schema.json: allowsLibraryAccess must be boolean")

    if not isinstance(fixture.get("bookmarks.json"), list):
        FAILURES.append("persisted-state/fixtures/library-v1.json: bookmarks.json must be an array")
    policy = fixture.get("bridge-policy.json")
    if not isinstance(policy, dict) or not isinstance(policy.get("allowsLibraryAccess"), bool):
        FAILURES.append("persisted-state/fixtures/library-v1.json: bridge-policy.json must carry allowsLibraryAccess")


def is_uuid(value: object) -> bool:
    if not isinstance(value, str):
        return False
    try:
        UUID(value)
    except ValueError:
        return False
    return True


def is_integer(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def is_iso_date(value: object) -> bool:
    if not isinstance(value, str):
        return False
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return False
    return parsed.tzinfo is not None


def is_persisted_date(value: object) -> bool:
    if isinstance(value, bool):
        return False
    if isinstance(value, (int, float)):
        return math.isfinite(value)
    return is_iso_date(value)


def validate_download_entry(entry: object, label: str, wire_date: bool):
    if not isinstance(entry, dict):
        FAILURES.append(f"{label}: download entry must be an object")
        return
    if set(entry) != DOWNLOAD_FIELDS:
        FAILURES.append(f"{label}: download entry must contain exactly {sorted(DOWNLOAD_FIELDS)}")
    if not is_uuid(entry.get("id")):
        FAILURES.append(f"{label}: id must be a UUID")
    for field in ["name", "source"]:
        if not isinstance(entry.get(field), str) or not entry[field]:
            FAILURES.append(f"{label}: {field} must be a non-empty string")
    date_valid = is_iso_date(entry.get("date")) if wire_date else is_persisted_date(entry.get("date"))
    if not date_valid:
        expected = "an ISO date-time string" if wire_date else "an ISO date-time string or finite number"
        FAILURES.append(f"{label}: date must be {expected}")
    if entry.get("state") not in DOWNLOAD_STATES:
        FAILURES.append(f"{label}: invalid download state")
    for field in ["received", "expected"]:
        if not is_integer(entry.get(field)):
            FAILURES.append(f"{label}: {field} must be an integer")
    for field in ["path", "error"]:
        if entry.get(field) is not None and not isinstance(entry.get(field), str):
            FAILURES.append(f"{label}: {field} must be a string or null")


def check_download_protocol_contract():
    fixture = load_json(CONTRACTS / "agent-protocol" / "fixtures" / "downloads.json")
    errors = load_json(CONTRACTS / "agent-protocol" / "fixtures" / "errors.json")
    if not isinstance(fixture, dict) or not isinstance(errors, dict):
        return

    request = fixture.get("downloadRequest")
    if not isinstance(request, dict) or request.get("command") != "download" or not str(request.get("url", "")).startswith(("http://", "https://")):
        FAILURES.append("agent-protocol/fixtures/downloads.json: invalid downloadRequest")
    if fixture.get("downloadResponseResult") != {"started": True, "next": DOWNLOAD_NEXT}:
        FAILURES.append("agent-protocol/fixtures/downloads.json: downloadResponseResult drifted")
    if fixture.get("downloadsRequest") != {"command": "downloads"}:
        FAILURES.append("agent-protocol/fixtures/downloads.json: downloadsRequest drifted")

    snapshot_entries: dict[str, dict] = {}
    for key, expected_state in [
        ("progressResponseResult", "downloading"),
        ("completedResponseResult", "completed"),
        ("cancelledResponseResult", "cancelled"),
        ("failedResponseResult", "failed"),
        ("interruptedResponseResult", "interrupted"),
    ]:
        result = fixture.get(key)
        entries = result.get("downloads") if isinstance(result, dict) else None
        if not isinstance(result, dict) or not isinstance(result.get("directory"), str) or not result["directory"]:
            FAILURES.append(f"agent-protocol/fixtures/downloads.json: {key} must include a directory")
        if not isinstance(entries, list) or len(entries) != 1:
            FAILURES.append(f"agent-protocol/fixtures/downloads.json: {key}.downloads must contain one entry")
            continue
        entry = entries[0]
        validate_download_entry(entry, f"agent-protocol/fixtures/downloads.json {key}", wire_date=True)
        if isinstance(entry, dict):
            snapshot_entries[key] = entry
            if entry.get("state") != expected_state:
                FAILURES.append(f"agent-protocol/fixtures/downloads.json: {key} state must be {expected_state}")

    progress = snapshot_entries.get("progressResponseResult")
    completed = snapshot_entries.get("completedResponseResult")
    cancelled = snapshot_entries.get("cancelledResponseResult")
    failed = snapshot_entries.get("failedResponseResult")
    interrupted = snapshot_entries.get("interruptedResponseResult")
    if progress is not None and not (0 < progress.get("received", 0) < progress.get("expected", 0)):
        FAILURES.append("agent-protocol/fixtures/downloads.json: progress snapshot must be partial")
    if completed is not None and completed.get("received") != completed.get("expected"):
        FAILURES.append("agent-protocol/fixtures/downloads.json: completed snapshot byte counts must match")
    for key, terminal in [("failedResponseResult", failed), ("interruptedResponseResult", interrupted)]:
        if terminal is not None and (terminal.get("path") is not None or not terminal.get("error")):
            FAILURES.append(f"agent-protocol/fixtures/downloads.json: {key} must hide staging and expose a reason")

    cancel_request = fixture.get("cancelDownloadRequest")
    cancel_result = fixture.get("cancelDownloadResponseResult")
    cancel_id = cancel_request.get("id") if isinstance(cancel_request, dict) else None
    if not isinstance(cancel_request, dict) or cancel_request.get("command") != "cancel-download" or not is_uuid(cancel_id):
        FAILURES.append("agent-protocol/fixtures/downloads.json: invalid cancelDownloadRequest")
    if cancel_result != {"cancelRequested": cancel_id}:
        FAILURES.append("agent-protocol/fixtures/downloads.json: cancelDownloadResponseResult must echo the request id")
    if progress is not None and progress.get("id") != cancel_id:
        FAILURES.append("agent-protocol/fixtures/downloads.json: progress id must match cancel request")
    if cancelled is not None and cancelled.get("id") != cancel_id:
        FAILURES.append("agent-protocol/fixtures/downloads.json: cancelled id must match cancel request")

    expected_commands = {
        "invalidDownloadURL": "download",
        "unknownDownload": "cancel-download",
        "inactiveDownload": "cancel-download",
    }
    for key, expected_error in DOWNLOAD_ERRORS.items():
        case = errors.get(key)
        case_request = case.get("request") if isinstance(case, dict) else None
        response = case.get("response") if isinstance(case, dict) else None
        if not isinstance(case_request, dict) or case_request.get("command") != expected_commands[key]:
            FAILURES.append(f"agent-protocol/fixtures/errors.json: {key} request drifted")
        if response != {"ok": False, "error": expected_error}:
            FAILURES.append(f"agent-protocol/fixtures/errors.json: {key} response drifted")


def check_fixture(path: str, required: list[str]):
    doc = load_json(CONTRACTS / path)
    if doc is None:
        return
    serialized = json.dumps(doc, ensure_ascii=False)
    for key in required:
        if key not in serialized:
            FAILURES.append(f"{path}: missing expected key '{key}'")


def main() -> int:
    check_all_contract_json()
    check_commands()
    check_protocol_contract_values()
    check_library_contract()
    check_download_protocol_contract()
    check_fixture("agent-protocol/fixtures/status.json", ["protocol", "commands"])
    check_fixture("agent-protocol/fixtures/tabs.json", ["owner", "agentActive"])
    check_fixture("agent-protocol/fixtures/read-click-fill.json", ["document", "ref", "Read again"])
    check_fixture("agent-protocol/fixtures/bridge-boundaries.json", [
        "openShadowRoots", "Frame content is not included.", "fileInputs", "bfcacheRestore"
    ])
    check_fixture("agent-protocol/fixtures/errors.json", ["Unbekannter Befehl", "Agent is busy"])
    check_fixture("persisted-state/fixtures/session-v1.json", ["tabs", "spaces", "folders", "splitPairs"])
    check_fixture("persisted-state/fixtures/profiles-v1.json", ["00000000-0000-0000-0000-000000000001", "activeID"])
    check_fixture("persisted-state/v1-session.schema.json", ["StoredTab", "interactionState"])
    check_fixture("sync/v1.schema.json", ["currentSpace", "5000"])
    for keep in ["Sources/YOBRO/ControlBridge.swift", "scripts/build.sh", "tests/Swift"]:
        if not (ROOT / keep).exists():
            FAILURES.append(f"missing WebKit reference path: {keep}")
    if FAILURES:
        print("PARITY CONTRACT GATE: FAIL")
        for failure in FAILURES:
            print(f"  - {failure}")
        return 1
    print("PARITY CONTRACT GATE: PASS (no web page launched)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
