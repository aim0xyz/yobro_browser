#!/usr/bin/env python3
"""Dependency-free MCP stdio adapter for YOBRO's local Unix socket."""

from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import sys
from typing import Any


SERVER_NAME = "yobro"
SERVER_VERSION = "0.1.0"
DEFAULT_SOCKET = Path.home() / "Library/Application Support/YOBRO/control.sock"


def schema(properties: dict[str, Any] | None = None, required: list[str] | None = None) -> dict[str, Any]:
    value: dict[str, Any] = {"type": "object", "properties": properties or {}, "additionalProperties": False}
    if required:
        value["required"] = required
    return value


TAB = {"type": "string", "description": "Optional YOBRO agent-tab UUID. Omit to use the current agent tab."}
URL = {"type": "string", "description": "An absolute http or https URL."}

TOOLS: list[dict[str, Any]] = [
    {
        "name": "status",
        "description": "Check whether the local YOBRO browser is running and agent access is enabled.",
        "inputSchema": schema(),
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "tabs",
        "description": "List YOBRO tabs and identify user-owned versus agent-owned tabs.",
        "inputSchema": schema(),
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "open_page",
        "description": "Open a URL in the current YOBRO agent tab, creating the agent pane when needed.",
        "inputSchema": schema({"url": URL, "tab": TAB}, ["url"]),
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": True},
    },
    {
        "name": "new_page",
        "description": "Create a separate YOBRO agent tab and optionally open a URL in it.",
        "inputSchema": schema({"url": URL}),
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": True},
    },
    {
        "name": "read_page",
        "description": "Read the loaded page's text, headings, and interactive elements. Returns a document UUID and element refs for later actions.",
        "inputSchema": schema({"tab": TAB}),
        "annotations": {"readOnlyHint": True, "openWorldHint": True},
    },
    {
        "name": "click",
        "description": "Click an element using a ref and document UUID from the latest read_page result.",
        "inputSchema": schema({"ref": {"type": "string"}, "document": {"type": "string"}, "tab": TAB}, ["ref", "document"]),
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": True},
    },
    {
        "name": "fill",
        "description": "Set a form field using a ref and document UUID from the latest read_page result. This does not submit the form.",
        "inputSchema": schema(
            {"ref": {"type": "string"}, "document": {"type": "string"}, "value": {"type": "string"}, "tab": TAB},
            ["ref", "document", "value"],
        ),
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": True},
    },
    {
        "name": "scroll",
        "description": "Scroll the current page vertically. Positive amounts scroll down and negative amounts scroll up.",
        "inputSchema": schema({"amount": {"type": "integer", "minimum": -5000, "maximum": 5000, "default": 600}, "tab": TAB}),
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": True},
    },
    {
        "name": "navigate",
        "description": "Navigate the YOBRO agent tab backward, forward, or reload it.",
        "inputSchema": schema({"direction": {"type": "string", "enum": ["back", "forward", "reload"]}, "tab": TAB}, ["direction"]),
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": True},
    },
    {
        "name": "find_on_page",
        "description": "Use YOBRO's native in-page text search.",
        "inputSchema": schema({"query": {"type": "string"}, "backwards": {"type": "boolean", "default": False}, "tab": TAB}, ["query"]),
        "annotations": {"readOnlyHint": True, "openWorldHint": True},
    },
    {
        "name": "history",
        "description": "Search the active YOBRO profile's local browsing history.",
        "inputSchema": schema({"query": {"type": "string", "default": ""}, "limit": {"type": "integer", "minimum": 1, "maximum": 200, "default": 50}}),
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "downloads",
        "description": "List downloads from the active YOBRO profile.",
        "inputSchema": schema(),
        "annotations": {"readOnlyHint": True, "openWorldHint": False},
    },
    {
        "name": "download",
        "description": "Start downloading an http or https URL through the active YOBRO WebKit session.",
        "inputSchema": schema({"url": URL, "tab": TAB}, ["url"]),
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": True},
    },
    {
        "name": "duplicate_page",
        "description": "Duplicate a YOBRO agent tab.",
        "inputSchema": schema({"tab": TAB}),
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": True},
    },
    {
        "name": "close_page",
        "description": "Close a YOBRO agent-owned tab. User-owned tabs remain protected.",
        "inputSchema": schema({"tab": TAB}),
        "annotations": {"readOnlyHint": False, "destructiveHint": True, "openWorldHint": True},
    },
    {
        "name": "end_session",
        "description": "End the YOBRO agent session and return its tabs to the normal tab list without disabling future access.",
        "inputSchema": schema(),
        "annotations": {"readOnlyHint": False, "destructiveHint": False, "openWorldHint": False},
    },
]


def socket_path() -> Path:
    configured = os.environ.get("YOBRO_SOCKET")
    return Path(configured).expanduser() if configured else DEFAULT_SOCKET


def yobro_request(payload: dict[str, Any]) -> dict[str, Any]:
    path = socket_path()
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(30)
            connection.connect(str(path))
            connection.sendall(json.dumps(payload, ensure_ascii=False).encode("utf-8") + b"\n")
            received = bytearray()
            while b"\n" not in received:
                chunk = connection.recv(65536)
                if not chunk:
                    raise RuntimeError("YOBRO closed the connection without a response.")
                received.extend(chunk)
                if len(received) > 8 * 1024 * 1024:
                    raise RuntimeError("YOBRO response exceeds 8 MB.")
        response = json.loads(received.split(b"\n", 1)[0])
        if not isinstance(response, dict):
            raise RuntimeError("YOBRO returned an invalid response.")
        return response
    except (OSError, ValueError, RuntimeError) as error:
        return {
            "ok": False,
            "error": str(error),
            "hint": "Start YOBRO and enable Agent Access. The default socket is " + str(path),
        }


def tool_payload(name: str, arguments: dict[str, Any]) -> dict[str, Any]:
    mappings: dict[str, str] = {
        "status": "status",
        "tabs": "tabs",
        "open_page": "open",
        "new_page": "new",
        "read_page": "read",
        "click": "click",
        "fill": "fill",
        "scroll": "scroll",
        "find_on_page": "find",
        "history": "history",
        "downloads": "downloads",
        "download": "download",
        "duplicate_page": "duplicate",
        "close_page": "close",
        "end_session": "end",
    }
    if name == "navigate":
        return {"command": arguments["direction"], **{key: value for key, value in arguments.items() if key != "direction"}}
    if name not in mappings:
        raise ValueError(f"Unknown tool: {name}")
    payload = {"command": mappings[name], **arguments}
    if name == "scroll" and "amount" not in payload:
        payload["amount"] = 600
    if name == "history":
        payload.setdefault("query", "")
        payload.setdefault("limit", 50)
    return payload


def call_tool(name: str, arguments: Any) -> dict[str, Any]:
    if not isinstance(arguments, dict):
        return {"content": [{"type": "text", "text": "Tool arguments must be an object."}], "isError": True}
    try:
        response = yobro_request(tool_payload(name, arguments))
    except (KeyError, TypeError, ValueError) as error:
        return {"content": [{"type": "text", "text": str(error)}], "isError": True}
    ok = response.get("ok") is True
    structured = response.get("result") if ok else response
    if not isinstance(structured, dict):
        structured = {"result": structured}
    result: dict[str, Any] = {
        "content": [{"type": "text", "text": json.dumps(structured, ensure_ascii=False, indent=2)}],
        "structuredContent": structured,
    }
    if not ok:
        result["isError"] = True
    return result


def respond(identifier: Any, result: Any = None, error: dict[str, Any] | None = None) -> None:
    message: dict[str, Any] = {"jsonrpc": "2.0", "id": identifier}
    if error is not None:
        message["error"] = error
    else:
        message["result"] = result
    sys.stdout.write(json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def handle(message: dict[str, Any]) -> None:
    method = message.get("method")
    identifier = message.get("id")
    if identifier is None:
        return
    if method == "initialize":
        requested = message.get("params", {}).get("protocolVersion", "2025-06-18")
        respond(
            identifier,
            {
                "protocolVersion": requested,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
                "instructions": (
                    "Use these tools whenever the user asks to use YOBRO. Work in YOBRO's protected agent tabs, "
                    "read before interacting, verify page changes with another read, and end the session when done."
                ),
            },
        )
    elif method == "ping":
        respond(identifier, {})
    elif method == "tools/list":
        respond(identifier, {"tools": TOOLS})
    elif method == "tools/call":
        params = message.get("params") or {}
        respond(identifier, call_tool(params.get("name", ""), params.get("arguments") or {}))
    else:
        respond(identifier, error={"code": -32601, "message": f"Method not found: {method}"})


def main() -> int:
    for raw_line in sys.stdin.buffer:
        try:
            value = json.loads(raw_line)
            if isinstance(value, dict):
                handle(value)
        except Exception as error:  # Keep protocol diagnostics off stdout.
            print(f"YOBRO MCP error: {error}", file=sys.stderr, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
