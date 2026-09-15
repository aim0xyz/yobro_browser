#!/usr/bin/env python3
"""Verify YOBRO 0.2 against real local HTTP responses and visible WebKit tabs."""
import http.server
import json
import os
from pathlib import Path
import threading
import time
from urllib.parse import urlparse
from integration import Fixture, PAGE, call, client, element

PAYLOAD = b"YOBRO download: authenticated, complete, and never overwritten.\n" * 500


class DailyFixture(Fixture):
    def do_GET(self):
        route = urlparse(self.path).path
        if route in ("/download", "/slow", "/broken"):
            if "yobro_fixture=shared" not in self.headers.get("Cookie", ""):
                self.send_error(403, "Missing browser session cookie")
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", 'attachment; filename="yobro-report.txt"')
            self.send_header("Content-Length", str(len(PAYLOAD) if route == "/download" else 5 * 1024 * 1024))
            self.end_headers()
            try:
                if route == "/download":
                    self.wfile.write(PAYLOAD)
                elif route == "/broken":
                    self.wfile.write(b"incomplete")
                    self.close_connection = True
                else:
                    for _ in range(80):
                        self.wfile.write(b"x" * 65536)
                        self.wfile.flush()
                        time.sleep(0.1)
            except (BrokenPipeError, ConnectionResetError):
                pass
        elif route == "/":
            body = PAGE.replace(b"</body>", b'<a href="/download" download>Download report</a></body>')
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            super().do_GET()


def wait_download(source, state, previous=None):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        items = call("downloads")["downloads"]
        item = next((d for d in items if d["source"] == source and d["id"] != previous), None)
        if item and item["state"] == state:
            return item
        if item and item["state"] in ("failed", "cancelled") and item["state"] != state:
            raise AssertionError(item)
        time.sleep(0.1)
    raise AssertionError(f"Download did not reach {state}: {items}")


def main():
    assert os.environ.get("YOBRO_HOME"), "Use isolated YOBRO_HOME."
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), DailyFixture)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_port}"
    original = next(t["id"] for t in call("tabs")["tabs"] if t["active"])
    created = []
    try:
        tab = call("new", url=base)
        created.append(tab["id"])
        assert call("find", query="YOBRO Test Garden")["found"]
        assert not call("find", query="absolutely-no-such-phrase-93284")["found"]
        assert call("find", query="garden", backwards=True)["found"]
        call("find", query="")
        assert call("history", query="YOBRO Test Garden")["history"]
        call("reload")
        visit = next(h for h in call("history")["history"] if h["url"].rstrip("/") == base)
        assert visit["visits"] >= 2
        assert not call("history", query="no-result-849831")["history"]
        print("PASS: native page search, no-match, backward search, persistent searchable history", flush=True)

        duplicate = call("duplicate")
        assert duplicate["id"] != tab["id"]
        created.append(duplicate["id"])
        assert call("read")["page"]["title"] == "YOBRO Test Garden"
        call("move", before=tab["id"])
        order = [t["id"] for t in call("tabs")["tabs"]]
        assert order.index(duplicate["id"]) < order.index(tab["id"])
        call("close")
        restored = call("restore")
        assert restored["id"] == duplicate["id"]
        assert call("read")["page"]["title"] == "YOBRO Test Garden"
        print("PASS: duplicate, reorder, close and restore real tab", flush=True)

        page = call("read")["page"]
        call("click", ref=element(page, "Download report")["ref"], document=page["document"])
        first = wait_download(base + "/download", "completed")
        first_path = Path(first["path"])
        assert first_path.read_bytes() == PAYLOAD
        assert first["received"] == len(PAYLOAD)
        assert call("read")["page"]["title"] == "YOBRO Test Garden"
        call("download", url=base + "/download")
        second = wait_download(base + "/download", "completed", previous=first["id"])
        assert second["path"] != first["path"]
        assert Path(second["path"]).read_bytes() == PAYLOAD
        assert first_path.read_bytes() == PAYLOAD
        print("PASS: authenticated link/command downloads, exact bytes, unique filenames, page preserved", flush=True)

        call("download", url=base + "/slow")
        slow = wait_download(base + "/slow", "downloading")
        call("cancel-download", id=slow["id"])
        cancelled = wait_download(base + "/slow", "cancelled")
        time.sleep(0.3)
        if cancelled.get("path"):
            assert not Path(cancelled["path"]).exists()
        assert not list(Path(call("downloads")["directory"]).glob("*.yobro-part"))
        call("download", url=base + "/broken")
        failed = wait_download(base + "/broken", "failed")
        assert failed.get("error")
        assert not Path(failed["path"]).exists()
        invalid = client.request({"command": "download", "url": "file:///etc/passwd"})
        assert not invalid["ok"]
        print("PASS: cancellation cleanup, truncated response failure, invalid download rejected", flush=True)

        for panel in ("history", "downloads", "palette", "none"):
            assert call("panel", value=panel)["panel"] == panel
            time.sleep(0.3)
        home = Path(os.environ["YOBRO_HOME"])
        assert json.loads((home / "history.json").read_text())
        saved = json.loads((home / "downloads.json").read_text())
        assert next(d for d in saved if d["id"] == first["id"])["state"] == "completed"
        print("PASS: panels and durable download/history records; ALL DAILY BROWSER CHECKS PASSED", flush=True)
    finally:
        call("panel", value="none")
        for identifier in created:
            call("close", tab=identifier)
        call("focus", tab=original)
        server.shutdown()


if __name__ == "__main__":
    main()
