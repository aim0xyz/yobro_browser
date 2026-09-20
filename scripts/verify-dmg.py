#!/usr/bin/env python3
"""Check a DMG and relocated MCP packages without opening real browser profiles."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def verify(image):
    with tempfile.TemporaryDirectory(prefix="yobro-dmg-check-") as temp:
        folder = Path(temp)
        mount = folder / "mounted"; mount.mkdir()
        attached = False
        try:
            run(["hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", str(mount), str(image)], stdout=subprocess.DEVNULL)
            attached = True
            assert (mount / "YoBro.app").is_dir()
            assert (mount / "Applications").is_symlink()
            assert os.readlink(mount / "Applications") == "/Applications"
            assert (mount / "INSTALL.txt").is_file()
            relocated = folder / "Applications/YoBro.app"
            relocated.parent.mkdir()
            run(["ditto", str(mount / "YoBro.app"), str(relocated)])
        finally:
            if attached: run(["hdiutil", "detach", str(mount)], stdout=subprocess.DEVNULL)

        run(["codesign", "--verify", "--deep", "--strict", str(relocated)])
        run([sys.executable, str(ROOT / "scripts/audit-distribution.py"), str(relocated)])
        integrations = relocated / "Contents/Resources/AgentIntegrations"
        claude = folder / "Claude extension"
        with zipfile.ZipFile(integrations / "YOBRO.mcpb") as archive:
            archive.extractall(claude)
            for info in archive.infolist():
                if not info.is_dir(): (claude / info.filename).chmod((info.external_attr >> 16) & 0o777)
        manifest = json.loads((claude / "manifest.json").read_text())
        assert manifest["server"]["type"] == "binary"
        assert manifest["server"]["mcp_config"]["args"] == ["--connection", "claude"]
        helpers = [relocated / "Contents/MacOS/yobro-mcp", claude / "bin/yobro-mcp",
                   integrations / "Codex/plugins/yobro-browser/bin/yobro-mcp"]
        for helper in helpers:
            run(["codesign", "--verify", "--strict", str(helper)])
            environment = dict(os.environ, YOBRO_TEST_MCP=str(helper))
            run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_distribution.py"],
                cwd=ROOT, env=environment)
        print("DMG, copied app, Claude extension and Codex adapter verified using synthetic profiles only.")


if __name__ == "__main__": verify(Path(sys.argv[1]).resolve())
