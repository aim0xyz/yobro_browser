#!/usr/bin/env python3
"""Validate the local macOS Chromium feasibility bundle recursively."""

from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any

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
MAX_PROTOCOL_RESPONSE_BYTES = 4 * 1024 * 1024
PROTOCOL_COMMAND_TIMEOUT_SECONDS = 30.0
PROTOCOL_STARTUP_TIMEOUT_SECONDS = 60.0
PROTOCOL_SHUTDOWN_TIMEOUT_SECONDS = 30.0
FIXTURE_BODY = b"""<!doctype html>
<html><head><meta charset="utf-8"><title>Packaged Protocol Fixture</title></head>
<body><label>Name<input id="name"></label>
<button id="apply" onclick="document.getElementById('out').textContent=document.getElementById('name').value">Apply</button>
<p id="out">Ready</p></body></html>"""
DOWNLOAD_BODY = b"YOBRO packaged download fixture\n"
DOWNLOAD_NAME = "packaged-protocol.bin"


class ProtocolFixtureHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        if self.path == "/download":
            body = DOWNLOAD_BODY
            content_type = "application/octet-stream"
            disposition = f'attachment; filename="{DOWNLOAD_NAME}"'
        else:
            body = FIXTURE_BODY
            content_type = "text/html; charset=utf-8"
            disposition = None
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        if disposition is not None:
            self.send_header("Content-Disposition", disposition)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format: str, *_args: object) -> None:
        return


def run(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, check=check, capture_output=True, text=True)


def is_macho(path: Path) -> bool:
    if not path.is_file():
        return False
    try:
        with path.open("rb") as stream:
            return stream.read(4) in MACHO_MAGICS
    except OSError:
        return False


def protocol_request(
    socket_path: Path,
    request: dict[str, Any],
    timeout: float = PROTOCOL_COMMAND_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    encoded = json.dumps(request, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"
    response = bytearray()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(str(socket_path))
        client.sendall(encoded)
        while b"\n" not in response:
            chunk = client.recv(64 * 1024)
            if not chunk:
                raise RuntimeError("control socket closed before a newline-terminated response")
            response.extend(chunk)
            if len(response) > MAX_PROTOCOL_RESPONSE_BYTES:
                raise RuntimeError("control socket response exceeded the validation limit")

    line = bytes(response).split(b"\n", 1)[0]
    try:
        decoded = json.loads(line.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"control socket returned invalid UTF-8 JSON: {error}") from error
    if not isinstance(decoded, dict):
        raise RuntimeError("control socket response must be a JSON object")
    return decoded


def protocol_result(response: dict[str, Any], command: str) -> dict[str, Any]:
    if response.get("ok") is not True:
        raise RuntimeError(f"{command} failed: {response.get('error', 'missing protocol error')}")
    result = response.get("result")
    if not isinstance(result, dict):
        raise RuntimeError(f"{command} returned a non-object result")
    return result


def wait_for_protocol_status(
    process: subprocess.Popen[bytes],
    socket_path: Path,
) -> dict[str, Any]:
    deadline = time.monotonic() + PROTOCOL_STARTUP_TIMEOUT_SECONDS
    last_error = "socket was not created"
    while time.monotonic() < deadline:
        return_code = process.poll()
        if return_code is not None:
            raise RuntimeError(f"packaged browser exited before protocol readiness (code {return_code})")
        try:
            mode = socket_path.lstat().st_mode
            if not stat.S_ISSOCK(mode):
                raise RuntimeError(f"expected a Unix socket at {socket_path}")
            status_result = protocol_result(
                protocol_request(socket_path, {"command": "status"}, timeout=2.0),
                "status",
            )
            expected = {
                "engine": "Chromium",
                "protocol": 2,
                "profile": "default",
                "socket": str(socket_path),
            }
            mismatches = {
                key: (status_result.get(key), value)
                for key, value in expected.items()
                if status_result.get(key) != value
            }
            if mismatches:
                raise RuntimeError(f"status identity mismatch: {mismatches}")
            return status_result
        except (OSError, RuntimeError) as error:
            last_error = str(error)
            time.sleep(0.1)
    raise RuntimeError(f"packaged protocol did not become ready: {last_error}")


def required_string(value: dict[str, Any], key: str, context: str) -> str:
    field = value.get(key)
    if not isinstance(field, str) or not field:
        raise RuntimeError(f"{context} omitted non-empty string field {key!r}")
    return field


def element_reference(page: dict[str, Any], tag: str) -> str:
    elements = page.get("elements")
    if not isinstance(elements, list):
        raise RuntimeError("read page omitted the elements array")
    for element in elements:
        if isinstance(element, dict) and element.get("tag") == tag:
            reference = element.get("ref")
            if isinstance(reference, str) and reference:
                return reference
    raise RuntimeError(f"read page omitted an actionable {tag} reference")


def exercise_packaged_protocol(
    process: subprocess.Popen[bytes],
    socket_path: Path,
    fixture_url: str,
    expected_download_directory: Path,
) -> None:
    status = wait_for_protocol_status(process, socket_path)
    commands = status.get("commands")
    required_commands = {
        "status", "new", "read", "fill", "click", "history", "download", "downloads", "tabs", "end"
    }
    if not isinstance(commands, list) or not required_commands.issubset(set(commands)):
        raise RuntimeError("status omitted commands required by the packaged protocol journey")
    if status.get("libraryAccess") is not True:
        raise RuntimeError("packaged startup did not load the persisted library-access opt-in before readiness")

    created = protocol_result(
        protocol_request(socket_path, {"command": "new", "url": fixture_url}),
        "new",
    )
    tab_id = required_string(created, "id", "new")
    if created.get("owner") != "agent" or created.get("url") != fixture_url:
        raise RuntimeError("new did not return the expected agent-owned fixture tab")

    history = protocol_result(
        protocol_request(socket_path, {"command": "history", "query": "", "limit": 20}),
        "history",
    ).get("history")
    if not isinstance(history, list) or not any(
        isinstance(entry, dict) and entry.get("url") == fixture_url for entry in history
    ):
        raise RuntimeError("packaged startup opt-in did not expose the completed fixture visit")
    downloads_result = protocol_result(
        protocol_request(socket_path, {"command": "downloads"}),
        "downloads",
    )
    download_directory = Path(required_string(downloads_result, "directory", "downloads"))
    if download_directory.resolve() != expected_download_directory.resolve():
        raise RuntimeError(
            "packaged protocol did not use its isolated temporary download directory"
        )
    download_url = fixture_url.rsplit("/", 1)[0] + "/download"
    started = protocol_result(
        protocol_request(
            socket_path,
            {"command": "download", "tab": tab_id, "url": download_url},
        ),
        "download",
    )
    if started != {
        "started": True,
        "next": "Use downloads to check progress and retrieve the final file path.",
    }:
        raise RuntimeError(f"download start result drifted: {started}")

    deadline = time.monotonic() + PROTOCOL_COMMAND_TIMEOUT_SECONDS
    packaged_download: dict[str, Any] | None = None
    last_state = "missing"
    while time.monotonic() < deadline:
        current = protocol_result(
            protocol_request(socket_path, {"command": "downloads"}),
            "downloads after start",
        )
        entries = current.get("downloads")
        if isinstance(entries, list):
            packaged_download = next(
                (
                    entry
                    for entry in entries
                    if isinstance(entry, dict) and entry.get("source") == download_url
                ),
                None,
            )
        if packaged_download is not None:
            last_state = str(packaged_download.get("state", "missing"))
            if last_state == "completed":
                break
            if last_state in {"failed", "cancelled", "interrupted"}:
                raise RuntimeError(
                    f"packaged download terminated as {last_state}: {packaged_download.get('error')}"
                )
        time.sleep(0.05)
    if packaged_download is None or packaged_download.get("state") != "completed":
        raise RuntimeError(f"packaged download did not complete; last state was {last_state}")
    if packaged_download.get("name") != DOWNLOAD_NAME:
        raise RuntimeError("packaged download filename differs")
    if packaged_download.get("received") != len(DOWNLOAD_BODY) or packaged_download.get("expected") != len(DOWNLOAD_BODY):
        raise RuntimeError("packaged download byte counters differ")
    if packaged_download.get("error") is not None:
        raise RuntimeError("packaged completed download has an error")
    download_path = Path(required_string(packaged_download, "path", "completed download"))
    if download_path.parent.resolve() != download_directory.resolve():
        raise RuntimeError("packaged download escaped the reported directory")
    if download_path.read_bytes() != DOWNLOAD_BODY:
        raise RuntimeError("packaged download bytes differ")

    first_read = protocol_result(
        protocol_request(socket_path, {"command": "read", "tab": tab_id}),
        "read",
    )
    page = first_read.get("page")
    if not isinstance(page, dict):
        raise RuntimeError("read omitted the page snapshot")
    document = required_string(page, "document", "read page")
    input_ref = element_reference(page, "input")
    button_ref = element_reference(page, "button")
    value = "Packaged controller works"

    protocol_result(
        protocol_request(
            socket_path,
            {
                "command": "fill",
                "tab": tab_id,
                "document": document,
                "ref": input_ref,
                "value": value,
            },
        ),
        "fill",
    )
    protocol_result(
        protocol_request(
            socket_path,
            {
                "command": "click",
                "tab": tab_id,
                "document": document,
                "ref": button_ref,
            },
        ),
        "click",
    )

    second_read = protocol_result(
        protocol_request(socket_path, {"command": "read", "tab": tab_id}),
        "read after fill/click",
    )
    second_page = second_read.get("page")
    if not isinstance(second_page, dict) or value not in str(second_page.get("text", "")):
        raise RuntimeError("fill/click/read did not update the packaged Chromium document")

    ended = protocol_result(protocol_request(socket_path, {"command": "end"}), "end")
    if ended.get("ended") is not True:
        raise RuntimeError("end did not confirm agent-workspace teardown")
    remaining = protocol_result(protocol_request(socket_path, {"command": "tabs"}), "tabs after end")
    tabs = remaining.get("tabs")
    if not isinstance(tabs, list) or any(
        isinstance(tab, dict) and tab.get("owner") == "agent" for tab in tabs
    ):
        raise RuntimeError("end left an agent-owned tab in the packaged session")


def close_child_stdin(process: subprocess.Popen[bytes]) -> None:
    if process.stdin is None or process.stdin.closed:
        return
    try:
        process.stdin.close()
    except (BrokenPipeError, OSError):
        pass


def terminate_process_group(process: subprocess.Popen[bytes]) -> None:
    close_child_stdin(process)
    if process.poll() is not None:
        return
    try:
        process.wait(timeout=10)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=10)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=10)


def log_tail(stream: Any, limit: int = 12_000) -> str:
    stream.flush()
    stream.seek(0)
    return stream.read()[-limit:]


def run_packaged_protocol_smoke(executable: Path) -> None:
    fixture = ThreadingHTTPServer(("127.0.0.1", 0), ProtocolFixtureHandler)
    fixture.daemon_threads = True
    fixture_thread = threading.Thread(
        target=fixture.serve_forever,
        name="yobro-packaged-protocol-fixture",
        daemon=True,
    )
    fixture_thread.start()
    port = int(fixture.server_address[1])
    fixture_url = f"http://127.0.0.1:{port}/fixture"

    failure: Exception | None = None
    process: subprocess.Popen[bytes] | None = None
    stdout_tail = ""
    stderr_tail = ""
    try:
        with tempfile.TemporaryDirectory(prefix="yb-pkg-home-", dir="/tmp") as home, tempfile.TemporaryDirectory(
            prefix="yb-pkg-profile-", dir="/tmp"
        ) as profile, tempfile.TemporaryFile(mode="w+", encoding="utf-8") as stdout_log, tempfile.TemporaryFile(
            mode="w+", encoding="utf-8"
        ) as stderr_log:
            socket_path = Path(profile) / "Profiles/default/control.sock"
            profile_parent = Path(profile) / "Profiles"
            profile_directory = profile_parent / "default"
            profile_parent.mkdir(mode=0o700)
            profile_directory.mkdir(mode=0o700)
            os.chmod(profile_parent, 0o700)
            os.chmod(profile_directory, 0o700)
            policy_path = profile_directory / "bridge-policy.json"
            policy_path.write_text(
                json.dumps({"allowsLibraryAccess": True}, separators=(",", ":")),
                encoding="utf-8",
            )
            os.chmod(policy_path, 0o600)
            environment = {
                "HOME": home,
                "TMPDIR": "/tmp",
                "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
                "YOBRO_CHROMIUM_HOME": profile,
            }
            try:
                process = subprocess.Popen(
                    [str(executable), "--package-protocol-smoke"],
                    env=environment,
                    stdin=subprocess.PIPE,
                    stdout=stdout_log,
                    stderr=stderr_log,
                    start_new_session=True,
                    close_fds=True,
                )
                exercise_packaged_protocol(
                    process,
                    socket_path,
                    fixture_url,
                    profile_directory / "package-protocol-downloads",
                )
                close_child_stdin(process)
                return_code = process.wait(timeout=PROTOCOL_SHUTDOWN_TIMEOUT_SECONDS)
                if return_code != 0:
                    raise RuntimeError(f"packaged browser did not shut down cleanly (code {return_code})")
                if socket_path.exists():
                    raise RuntimeError("packaged browser left its control socket after clean shutdown")
                persisted_policy = json.loads(policy_path.read_text(encoding="utf-8"))
                if persisted_policy != {"allowsLibraryAccess": True}:
                    raise RuntimeError("packaged browser changed the persisted bridge policy")
                if stat.S_IMODE(policy_path.stat().st_mode) != 0o600:
                    raise RuntimeError("packaged bridge policy is not owner-only")
                downloads_path = profile_directory / "downloads.json"
                persisted_downloads = json.loads(downloads_path.read_text(encoding="utf-8"))
                download_url = fixture_url.rsplit("/", 1)[0] + "/download"
                matching = [
                    entry for entry in persisted_downloads
                    if isinstance(entry, dict) and entry.get("source") == download_url
                ] if isinstance(persisted_downloads, list) else []
                if len(matching) != 1:
                    raise RuntimeError("packaged completed download was not persisted exactly once")
                saved = matching[0]
                if (saved.get("state") != "completed" or saved.get("name") != DOWNLOAD_NAME
                    or saved.get("received") != len(DOWNLOAD_BODY)
                    or saved.get("expected") != len(DOWNLOAD_BODY)
                    or saved.get("error") is not None):
                    raise RuntimeError(f"packaged persisted download metadata differs: {saved}")
                saved_path = Path(required_string(saved, "path", "persisted download"))
                if (saved_path.parent.resolve() != (profile_directory / "package-protocol-downloads").resolve()
                    or saved_path.read_bytes() != DOWNLOAD_BODY):
                    raise RuntimeError("packaged persisted download path or bytes differ")

                process = subprocess.Popen(
                    [str(executable), "--package-protocol-smoke"],
                    env=environment,
                    stdin=subprocess.PIPE,
                    stdout=stdout_log,
                    stderr=stderr_log,
                    start_new_session=True,
                    close_fds=True,
                )
                restarted_status = wait_for_protocol_status(process, socket_path)
                if restarted_status.get("libraryAccess") is not True:
                    raise RuntimeError("packaged restart lost the persisted library-access policy")
                reopened = protocol_result(
                    protocol_request(socket_path, {"command": "downloads"}),
                    "downloads after restart",
                )
                reopened_entries = reopened.get("downloads")
                reopened_matches = [
                    entry for entry in reopened_entries
                    if isinstance(entry, dict) and entry.get("source") == download_url
                ] if isinstance(reopened_entries, list) else []
                if len(reopened_matches) != 1 or reopened_matches[0] != saved:
                    raise RuntimeError("packaged restart did not restore the exact completed download")
                close_child_stdin(process)
                restart_code = process.wait(timeout=PROTOCOL_SHUTDOWN_TIMEOUT_SECONDS)
                if restart_code != 0 or socket_path.exists():
                    raise RuntimeError("packaged restart did not shut down cleanly")
            except Exception as error:
                failure = error
            finally:
                if process is not None:
                    terminate_process_group(process)
                stdout_tail = log_tail(stdout_log)
                stderr_tail = log_tail(stderr_log)
    finally:
        fixture.shutdown()
        fixture.server_close()
        fixture_thread.join(timeout=5)

    if failure is not None:
        raise RuntimeError(
            f"{failure}\npackaged stdout tail:\n{stdout_tail or '<empty>'}"
            f"\npackaged stderr tail:\n{stderr_tail or '<empty>'}"
        ) from failure


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    app = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else (
        root / "dist/chromium-macos-arm64/YOBRO Chromium Feasibility.app"
    )
    contents = app / "Contents"
    executable = contents / "MacOS/YOBRO Chromium Feasibility"
    webengine = contents / "Frameworks/QtWebEngineCore.framework/Versions/A"
    helper = webengine / "Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess"
    resources = webengine / "Resources"
    licenses = contents / "Resources/licenses"

    required = [
        executable,
        helper,
        resources / "icudtl.dat",
        resources / "qtwebengine_resources.pak",
        resources / "qtwebengine_devtools_resources.pak",
        resources / "v8_context_snapshot.arm64.bin",
        resources / "qtwebengine_locales/en-US.pak",
        contents / "PlugIns/platforms/libqcocoa.dylib",
        licenses / "THIRD-PARTY-NOTICES.md",
        licenses / "LICENSE.Chromium",
        licenses / "qtwebengine-6.11.2.spdx",
    ]
    errors = [f"Missing required bundle item: {path}" for path in required if not path.exists()]

    for path in contents.rglob("*"):
        if path.is_symlink() and not path.exists():
            errors.append(f"Broken symlink: {path} -> {os.readlink(path)}")

    macho_files = [
        path for path in contents.rglob("*") if not path.is_symlink() and is_macho(path)
    ]
    for path in macho_files:
        archs = run("/usr/bin/lipo", "-archs", str(path)).stdout.strip().split()
        if "arm64" not in archs:
            errors.append(f"Mach-O does not contain arm64: {path} ({' '.join(archs)})")

        linkage = run("/usr/bin/otool", "-L", str(path)).stdout.splitlines()[1:]
        install_id_result = run("/usr/bin/otool", "-D", str(path), check=False)
        install_ids = {
            line.strip()
            for line in install_id_result.stdout.splitlines()[1:]
            if line.strip()
        }
        for line in linkage:
            dependency = line.strip().split(" ", 1)[0]
            if dependency in install_ids:
                continue
            if dependency.startswith("/") and not dependency.startswith(ALLOWED_ABSOLUTE_PREFIXES):
                errors.append(f"External dependency in {path}: {dependency}")

        load_commands = run("/usr/bin/otool", "-l", str(path)).stdout.splitlines()
        for index, line in enumerate(load_commands):
            if line.strip() != "cmd LC_RPATH":
                continue
            for candidate in load_commands[index + 1 : index + 6]:
                stripped = candidate.strip()
                if not stripped.startswith("path "):
                    continue
                rpath = stripped.split()[1]
                if rpath.startswith("/") and not rpath.startswith(ALLOWED_ABSOLUTE_PREFIXES):
                    errors.append(f"External LC_RPATH in {path}: {rpath}")
                break

    locales = list((resources / "qtwebengine_locales").glob("*.pak"))
    if len(locales) < 50:
        errors.append(f"Expected the complete Chromium locale set, found only {len(locales)} files")

    signature = run(
        "/usr/bin/codesign", "--verify", "--deep", "--strict", "--verbose=2", str(app), check=False
    )
    if signature.returncode != 0:
        errors.append(f"Code-signature verification failed: {signature.stderr.strip()}")

    if not errors:
        with tempfile.TemporaryDirectory(prefix="yobro-package-home-") as home, tempfile.TemporaryDirectory(
            prefix="yobro-package-profile-"
        ) as profile:
            environment = {
                "HOME": home,
                "TMPDIR": "/tmp",
                "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
                "YOBRO_CHROMIUM_HOME": profile,
            }
            try:
                smoke = subprocess.run(
                    [str(executable), "--smoke"],
                    env=environment,
                    capture_output=True,
                    text=True,
                    timeout=240,
                )
                if smoke.returncode != 0:
                    errors.append(
                        "Packaged smoke test failed with a clean environment:\n"
                        f"stdout:\n{smoke.stdout}\nstderr:\n{smoke.stderr}"
                    )
            except subprocess.TimeoutExpired as error:
                errors.append(f"Packaged smoke test timed out after {error.timeout} seconds")

    packaged_protocol_passed = False
    if not errors:
        try:
            run_packaged_protocol_smoke(executable)
            packaged_protocol_passed = True
        except Exception as error:
            errors.append(f"Packaged normal-mode protocol smoke failed:\n{error}")

    if errors:
        print("CHROMIUM PACKAGE GATE: FAIL", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1

    size = sum(
        path.stat().st_size
        for path in contents.rglob("*")
        if path.is_file() and not path.is_symlink()
    )
    if packaged_protocol_passed:
        print(
            "CHROMIUM PACKAGED PROTOCOL GATE: PASS "
            "(persisted-policy startup; normal session/controller/AF_UNIX "
            "status-history-download-new-read-fill-click-read-end; exact download restored after restart)"
        )
    print(
        "CHROMIUM PACKAGE GATE: PASS "
        f"({len(macho_files)} Mach-O files, {len(locales)} locales, {size / 1024 / 1024:.1f} MiB)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
