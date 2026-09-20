#!/usr/bin/env python3
"""Build distributable integrations from an explicit file list; never read user state."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def sign(path):
    identity = os.environ.get("YOBRO_SIGN_IDENTITY") or "-"
    args = ["codesign", "--force", "--sign", identity]
    if identity != "-":
        args += ["--options", "runtime", "--timestamp"]
    subprocess.run(args + [str(path)], check=True)
    subprocess.run(["codesign", "--verify", "--strict", str(path)], check=True)


def install_helper(destination, build):
    destination.mkdir(parents=True, exist_ok=True)
    executable = destination / "yobro-mcp"
    shutil.copy2(build / "yobro-mcp", executable)
    executable.chmod(0o755)
    sign(executable)
    # SwiftPM resolves resource bundles next to standalone executables.
    bundle = destination / "YOBRO_YOBROMCP.bundle"
    bundle.mkdir()
    shutil.copy2(ROOT / "Sources/YOBROMCP/Resources/tools.json", bundle / "tools.json")


def build_integrations(app, build):
    output = app / "Contents/Resources/AgentIntegrations"
    codex = output / "Codex"
    source = ROOT / "packaging/AgentIntegrations/Codex"
    plugin = Path("plugins/yobro-browser")
    allowed = [Path(".agents/plugins/marketplace.json"), plugin / ".codex-plugin/plugin.json",
               plugin / ".mcp.json", plugin / "skills/use-yobro/SKILL.md"]
    for relative in allowed:
        destination = codex / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / relative, destination)
    install_helper(codex / plugin / "bin", build)
    with tempfile.TemporaryDirectory(prefix="yobro-mcpb-") as temporary:
        root = Path(temporary)
        shutil.copy2(ROOT / "packaging/claude-manifest.json", root / "manifest.json")
        install_helper(root / "bin", build)
        with zipfile.ZipFile(output / "YOBRO.mcpb", "w", zipfile.ZIP_DEFLATED) as archive:
            for file in sorted(root.rglob("*")):
                if file.is_file():
                    archive.write(file, file.relative_to(root))


if __name__ == "__main__":
    build_integrations(Path(sys.argv[1]), Path(sys.argv[2]))
