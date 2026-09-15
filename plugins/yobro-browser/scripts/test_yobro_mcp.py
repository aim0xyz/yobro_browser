#!/usr/bin/env python3
"""Small protocol and socket-adapter smoke test for the YOBRO MCP server."""

from __future__ import annotations

import json
import os
from pathlib import Path
import queue
import socket
import subprocess
import tempfile
import threading


ROOT = Path(__file__).resolve().parent.parent
SERVER = ROOT / "scripts/yobro_mcp.py"


def fake_yobro(
    path: Path,
) -> tuple[threading.Thread, threading.Event, queue.Queue[Exception]]:
    ready = threading.Event()
    errors: queue.Queue[Exception] = queue.Queue(maxsize=1)

    def serve() -> None:
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(str(path))
                listener.listen(2)
                ready.set()
                for _ in range(2):
                    connection, _ = listener.accept()
                    with connection:
                        request = json.loads(connection.makefile("rb").readline())
                        result = {"echo": request, "browser": "YOBRO", "enabled": True}
                        connection.sendall(json.dumps({"ok": True, "result": result}).encode() + b"\n")
        except Exception as error:
            errors.put(error)
            ready.set()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    return thread, ready, errors


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "control.sock"
        thread, ready, errors = fake_yobro(path)
        if not ready.wait(timeout=2):
            raise TimeoutError("fake YOBRO server did not start within 2 seconds")
        if not errors.empty():
            raise RuntimeError("fake YOBRO server failed to start") from errors.get()
        messages = [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "status", "arguments": {}}},
            {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "open_page", "arguments": {"url": "https://example.com"}}},
        ]
        process = subprocess.run(
            ["python3", str(SERVER)],
            input="".join(json.dumps(message) + "\n" for message in messages),
            text=True,
            capture_output=True,
            env={**os.environ, "YOBRO_SOCKET": str(path)},
            check=True,
            timeout=10,
        )
        responses = [json.loads(line) for line in process.stdout.splitlines()]
        assert [response["id"] for response in responses] == [1, 2, 3, 4]
        assert responses[0]["result"]["serverInfo"]["name"] == "yobro"
        names = {tool["name"] for tool in responses[1]["result"]["tools"]}
        assert {"status", "open_page", "read_page", "click", "fill", "end_session"} <= names
        assert responses[2]["result"]["structuredContent"]["echo"] == {"command": "status"}
        assert responses[3]["result"]["structuredContent"]["echo"] == {"command": "open", "url": "https://example.com"}
        thread.join(1)
        assert not thread.is_alive()
    print("YOBRO MCP smoke test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
