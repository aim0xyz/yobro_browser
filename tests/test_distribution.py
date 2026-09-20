import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import shutil
import threading
import unittest
import zipfile

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("audit", ROOT / "scripts/audit-distribution.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class DistributionAuditTests(unittest.TestCase):
    def test_nested_archives_are_scanned_without_echoing_secrets(self):
        secret = "sk-" + "synthetic" * 6
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            nested = io.BytesIO()
            with zipfile.ZipFile(nested, "w") as archive:
                archive.writestr("config.txt", secret)
            with zipfile.ZipFile(root / "extension.mcpb", "w") as archive:
                archive.writestr("nested.zip", nested.getvalue())
            result = audit.audit(root)
            self.assertFalse(result["passed"])
            self.assertNotIn(secret, json.dumps(result))
            self.assertIn("provider-secret", str(result["failures"]))

    def test_private_state_and_symlinks_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "profiles.json").write_text("{}")
            (root / "linked").symlink_to("/tmp")
            result = audit.audit(root)
            self.assertEqual({f["rule"] for f in result["failures"]}, {"private-state-file", "unexpected-symlink"})

    def test_public_supabase_key_is_not_a_secret(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "app").write_text("sb_publishable_" + "public" * 6)
            self.assertTrue(audit.audit(root)["passed"])


class NativeMCPTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.binary = Path(os.environ.get("YOBRO_TEST_MCP", ROOT / ".build/debug/yobro-mcp"))
        if not cls.binary.exists(): raise unittest.SkipTest("Build yobro-mcp first")

    def run_messages(self, root, messages, extra_env=None):
        env = dict(os.environ)
        env.pop("YOBRO_SOCKET", None)
        env["YOBRO_HOME"] = str(root)
        env.update(extra_env or {})
        result = subprocess.run([str(self.binary), "--connection", "claude"],
                                input="".join(json.dumps(m) + "\n" for m in messages),
                                text=True, capture_output=True, env=env, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return [json.loads(line) for line in result.stdout.splitlines()]

    def call(self, name, arguments, id=1):
        return {"jsonrpc": "2.0", "id": id, "method": "tools/call", "params": {"name": name, "arguments": arguments}}

    def test_protocol_and_schema_match_existing_adapter(self):
        with tempfile.TemporaryDirectory() as tmp:
            responses = self.run_messages(tmp, [{"id": 1, "method": "initialize", "params": {"protocolVersion": "bogus"}}, {"method": "notifications/initialized"}, {"id": 2, "method": "tools/list"}])
            self.assertEqual(len(responses), 2)
            self.assertEqual(responses[0]["result"]["protocolVersion"], "2025-06-18")
            spec = importlib.util.spec_from_file_location("legacy", ROOT / "plugins/yobro-browser/scripts/yobro_mcp.py")
            legacy = importlib.util.module_from_spec(spec); spec.loader.exec_module(legacy)
            self.assertEqual(responses[1]["result"]["tools"], legacy.TOOLS)

    def test_unbound_agent_never_falls_back_to_personal_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.run_messages(tmp, [self.call("status", {})])[0]["result"]
            self.assertTrue(result["isError"])
            self.assertIn("connect this agent", result["content"][0]["text"])

    def test_invalid_input_is_rejected_before_any_socket_access(self):
        messages = [self.call("scroll", {"amount": True}), self.call("scroll", {"amount": 5001}),
                    self.call("navigate", {"direction": "delete"}), self.call("fill", {"ref": "e1"}),
                    self.call("status", {"command": "read"}), self.call("status", []),
                    self.call("scroll", {"amount": "600"})]
        with tempfile.TemporaryDirectory() as tmp:
            for response in self.run_messages(tmp, messages):
                self.assertTrue(response["result"]["isError"])
                self.assertNotIn("connect this agent", response["result"]["content"][0]["text"])

    def test_profile_binding_routes_only_to_selected_socket(self):
        # Short path stays within macOS's 104-byte Unix socket limit.
        with tempfile.TemporaryDirectory(dir="/tmp", prefix="ym-") as tmp:
            root = Path(tmp)
            (root / "AgentConnections").mkdir()
            name = "p-11111111-1111-1111-1111-111111111111.sock"
            (root / "AgentConnections/claude.json").write_text(json.dumps({"socketName": name}))
            received = []
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                server.bind(str(root / name)); (root / name).chmod(0o600); server.listen(1); server.settimeout(5)
                def respond():
                    connection, _ = server.accept()
                    with connection:
                        received.append(json.loads(connection.makefile("rb").readline()))
                        connection.sendall(b'{"ok":true,"result":{"enabled":true}}\n')
                thread = threading.Thread(target=respond); thread.start()
                response = self.run_messages(root, [self.call("scroll", {})])[0]
                thread.join(timeout=6)
            self.assertEqual(received, [{"command": "scroll", "amount": 600}])
            self.assertFalse(response["result"]["isError"])

    def test_binding_cannot_escape_profile_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); (root / "AgentConnections").mkdir()
            (root / "AgentConnections/claude.json").write_text('{"socketName":"../../other.sock"}')
            self.assertTrue(self.run_messages(root, [self.call("status", {})])[0]["result"]["isError"])

    def test_binary_never_falls_back_to_developer_resource_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "yobro-mcp"
            shutil.copy2(self.binary, executable)
            result = subprocess.run([str(executable)], input='', text=True, capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("resources are missing", result.stderr)


if __name__ == "__main__": unittest.main()
