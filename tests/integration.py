#!/usr/bin/env python3
"""End-to-end tests against a running isolated YOBRO instance."""
import http.server
import importlib.machinery
import json
import os
from pathlib import Path
import threading
import time
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[1]
client = importlib.machinery.SourceFileLoader("yobro_client", str(ROOT / "bin/yobro")).load_module()

PAGE = b'''<!doctype html><html><head><title>YOBRO Test Garden</title></head>
<body style="font:18px system-ui;padding:40px;background:#f6f6ef">
<h1>YOBRO Test Garden</h1><p>A real WebKit page controlled directly.</p>
<form action="/result"><label for="name">Your name</label><input id="name" name="name">
<label for="password">Password</label><input id="password" type="password" value="SECRET-MUST-BE-REDACTED">
<label><input type="checkbox" id="check"> Remember this</label>
<button type="submit">Grow something</button></form>
<button disabled>Disabled action</button><a href="/second">Second page</a>
<div id="shadow"></div><p id="shadow-result"></p>
<script>
document.cookie='yobro_fixture=shared; path=/';
window.__yobro={snapshot:()=>({poisoned:true})};
const shadow=document.querySelector('#shadow').attachShadow({mode:'open'});
shadow.innerHTML='<button>Shadow action</button>';
shadow.querySelector('button').onclick=()=>document.querySelector('#shadow-result').textContent='Shadow worked';
</script></body></html>'''


class Fixture(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/result":
            name = parse_qs(parsed.query).get("name", [""])[0]
            import html
            body = f"<html><title>Garden result</title><body><h1>Hello {html.escape(name)}</h1><p>Form submitted successfully.</p><a href='/'>Start again</a></body></html>".encode()
        elif parsed.path == "/cookies":
            body = ("<html><title>Cookie check</title><body>" + self.headers.get("Cookie", "No cookie") + "</body></html>").encode()
        elif parsed.path == "/second":
            body = b"<html><title>Second page</title><body><h1>A different document</h1><a href='/'>Back to garden</a></body></html>"
        else:
            body = PAGE
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def call(command, **kwargs):
    response = client.request({"command": command, **kwargs})
    assert response["ok"], response
    return response["result"]


def element(page, label):
    return next(item for item in page["elements"] if item["label"] == label)


def main():
    assert os.environ.get("YOBRO_HOME"), "Use an isolated YOBRO_HOME for tests."
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}"
    created = []
    try:
        status = call("status")
        assert status["engine"] == "WebKit" and status["enabled"]
        assert (Path(os.environ["YOBRO_HOME"]) / "control.sock").stat().st_mode & 0o777 == 0o600
        print("PASS: native WebKit connection and private socket", flush=True)

        original = call("tabs")
        original_id = next(tab["id"] for tab in original["tabs"] if tab["active"])
        tab = call("new", url=base)
        created.append(tab["id"])
        page = call("read")["page"]
        assert page["title"] == "YOBRO Test Garden" and not page.get("poisoned")
        assert "SECRET-MUST-BE-REDACTED" not in json.dumps(page)
        assert page["document"] == call("read")["page"]["document"]
        print("PASS: navigation, isolated DOM read, password redaction", flush=True)

        doc = page["document"]
        field = element(page, "Your name")
        name = 'Laura "YOBRO" \\ garden ☀'
        call("fill", ref=field["ref"], document=doc, value=name)
        reread = call("read")["page"]
        assert element(reread, "Your name")["value"] == name
        assert element(reread, "Your name")["ref"] == field["ref"]
        call("click", ref=element(page, "Remember this")["ref"], document=doc)
        assert element(call("read")["page"], "Remember this")["checked"]
        call("click", ref=element(page, "Shadow action")["ref"], document=doc)
        assert "Shadow worked" in call("read")["page"]["text"]
        blocked = client.request({"command": "click", "ref": element(page, "Disabled action")["ref"], "document": doc})
        assert not blocked["ok"] and "disabled" in blocked["error"], blocked
        print("PASS: Unicode/quoted input, checkbox, shadow DOM, disabled controls", flush=True)

        call("click", ref=element(page, "Grow something")["ref"], document=doc)
        result = call("read")["page"]
        assert f"Hello {name}" in result["text"], result
        rejected = client.request({"command": "click", "ref": field["ref"], "document": doc})
        assert not rejected["ok"] and "changed" in rejected["error"]
        print("PASS: real form submission and stale-document rejection", flush=True)

        second = call("new", url=base + "/cookies")
        created.append(second["id"])
        assert "yobro_fixture=shared" in call("read")["page"]["text"]
        call("focus", tab=tab["id"])
        assert call("read")["tab"]["id"] == tab["id"]
        call("pin")
        assert next(t for t in call("tabs")["tabs"] if t["id"] == tab["id"])["pinned"]
        call("split")
        call("split")
        call("space", value="Studio")
        call("space", value="Persönlich")
        print("PASS: shared cookie session, tab focus, pinning, split, spaces", flush=True)

        denied = client.request({"command": "open", "url": "file:///etc/passwd"})
        assert not denied["ok"]
        unknown = client.request({"command": "read", "tab": "missing"})
        assert not unknown["ok"]
        noeval = client.request({"command": "eval", "script": "document.cookie"})
        assert not noeval["ok"]
        print("PASS: invalid tab/scheme and arbitrary evaluation rejected", flush=True)
        call("focus", tab=original_id)
        time.sleep(0.4)
        stored = json.loads((Path(os.environ["YOBRO_HOME"]) / "session.json").read_text())
        assert stored["activeID"].lower() == original_id
        print("PASS: session persistence; ALL INTEGRATION CHECKS PASSED", flush=True)
    finally:
        for identifier in created:
            call("close", tab=identifier)
        server.shutdown()


if __name__ == "__main__":
    main()
