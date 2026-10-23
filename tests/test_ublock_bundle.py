import datetime
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


prepare = load("prepare_bundled_ublock", "prepare-bundled-ublock.py")
freshness = load("check_ublock_freshness", "check-ublock-freshness.py")
TAG = "2026.1028.905"


def upstream(tag=TAG, engine=prepare.ENGINE_CHECK, permissions=("storage",)):
    """A minimal stand-in for an official uBOL Safari release."""
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("manifest.json", json.dumps({"version": tag, "permissions": list(permissions), "host_permissions": ["<all_urls>"]}))
        archive.writestr("js/ext.js", b"function flavor(extURL) {\n    " + engine + b"\n}\n")
    return output.getvalue()


def read(package, name):
    with zipfile.ZipFile(io.BytesIO(package)) as archive:
        return archive.read(name)


class PrepareBundledUBlockTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.output = self.directory / "bundled.zip"
        self.notice = self.directory / "NOTICE.txt"

    def run_main(self, package, *extra, sha=None):
        source = self.directory / "upstream.zip"
        source.write_bytes(package)
        prepare.main(["--tag", TAG, "--sha256", sha or hashlib.sha256(package).hexdigest(), "--source", str(source),
                      "--output", str(self.output), "--notice", str(self.notice), *extra])

    def test_tag_date(self):
        self.assertEqual(prepare.tag_date("2026.914.1325"), (2026, 9, 14))
        self.assertEqual(prepare.tag_date("2026.1006.1931"), (2026, 10, 6))
        with self.assertRaises(ValueError):
            prepare.tag_date("v2026.1006")

    def test_patch_recognizes_webkit_and_is_reproducible(self):
        patched = prepare.patch(upstream(), TAG)
        self.assertEqual(json.loads(read(patched, "manifest.json"))["version"], f"{TAG}.{prepare.PATCH}")
        self.assertIn(prepare.ENGINE_PATCH, read(patched, "js/ext.js"))
        self.assertNotIn(prepare.ENGINE_CHECK, read(patched, "js/ext.js"))
        self.assertIn(TAG.encode(), read(patched, "YOBRO-COMPATIBILITY.txt"))
        self.assertEqual(prepare.patch(upstream(), TAG), patched)

    def test_changed_engine_detection_needs_review(self):
        with self.assertRaises(SystemExit):
            prepare.patch(upstream(engine=b"return 'safari';"), TAG)

    def test_manifest_must_match_tag(self):
        with self.assertRaises(SystemExit):
            prepare.patch(upstream(tag="2026.1021.1354"), TAG)

    def test_digest_is_verified_before_anything_is_written(self):
        with self.assertRaises(SystemExit):
            self.run_main(upstream(), sha="0" * 64)
        self.assertFalse(self.output.exists())
        self.assertFalse(self.notice.exists())

    def test_notice_names_release_and_both_digests(self):
        package = upstream()
        self.run_main(package)
        notice = self.notice.read_text()
        self.assertIn(f"Based on official Safari release: {TAG}\n", notice)
        self.assertIn(f"YoBro compatibility version: {TAG}.{prepare.PATCH}\n", notice)
        self.assertIn(f"{prepare.RELEASES}/tag/{TAG}\n", notice)
        self.assertIn(f"Upstream SHA-256: {hashlib.sha256(package).hexdigest()}\n", notice)
        self.assertIn(f"Bundled SHA-256: {hashlib.sha256(self.output.read_bytes()).hexdigest()}\n", notice)

    def test_permission_changes_need_acknowledgement(self):
        previous = prepare.patch(upstream(tag=TAG), TAG)
        self.output.write_bytes(previous)
        wider = upstream(permissions=("storage", "tabs"))
        with self.assertRaises(SystemExit):
            self.run_main(wider)
        self.assertEqual(self.output.read_bytes(), previous)
        self.run_main(wider, "--accept-permission-changes")
        self.assertIn("tabs", prepare.manifest(self.output.read_bytes())["permissions"])

    def test_committed_bundle_is_patched_official_release(self):
        package = (prepare.RESOURCES / "uBlockOriginLite.safari.zip").read_bytes()
        tag = freshness.bundled_tag(prepare.RESOURCES / "uBlockOriginLite.safari.zip")
        self.assertEqual(prepare.manifest(package)["version"], f"{tag}.{prepare.PATCH}")
        self.assertIn(prepare.ENGINE_PATCH, read(package, "js/ext.js"))
        notice = (prepare.RESOURCES / "uBlockOriginLite-NOTICE.txt").read_text()
        self.assertIn(f"Bundled SHA-256: {hashlib.sha256(package).hexdigest()}\n", notice)


class UBlockFreshnessTests(unittest.TestCase):
    def test_minimum_from_issue_checklist(self):
        body = "- [x] I updated to the latest versions (`2026.1018.2229`+)\n- [x] Older: `2026.930.1227`+"
        self.assertEqual(freshness.minimum_from_issue(body), "2026.1018.2229")
        self.assertIsNone(freshness.minimum_from_issue("Please update to the latest version."))
        self.assertIsNone(freshness.minimum_from_issue(None))

    def test_versions_compare_numerically(self):
        self.assertEqual(freshness.evaluate("2026.1006.1931", "2026.930.1227", "2026.930.1227")[0], freshness.CURRENT)

    def test_evaluate(self):
        latest, minimum = "2026.1021.1354", "2026.1018.2229"
        self.assertEqual(freshness.evaluate(latest, latest, minimum)[0], freshness.CURRENT)
        self.assertEqual(freshness.evaluate(minimum, latest, minimum)[0], freshness.BEHIND)
        self.assertEqual(freshness.evaluate("2026.914.1325", latest, minimum)[0], freshness.BELOW_MINIMUM)
        self.assertEqual(freshness.evaluate(latest, latest, None)[0], freshness.BEHIND)

    def test_offline_age_check(self):
        with tempfile.TemporaryDirectory() as temp:
            bundle = Path(temp) / "bundled.zip"
            today = datetime.date.today()
            fresh = f"{today.year}.{today.month}{today.day:02}.1200"
            bundle.write_bytes(prepare.patch(upstream(tag=fresh), fresh))
            self.assertEqual(freshness.bundled_tag(bundle), fresh)
            self.assertEqual(freshness.main(["--offline", "--bundle", str(bundle)]), freshness.CURRENT)
            bundle.write_bytes(prepare.patch(upstream(tag="2026.914.1325"), "2026.914.1325"))
            self.assertEqual(freshness.tag_age_days("2026.914.1325", datetime.date(2026, 9, 28)), 14)
            self.assertEqual(freshness.main(["--offline", "--bundle", str(bundle), "--max-age-days", "0"]), freshness.BEHIND)


if __name__ == "__main__":
    unittest.main()
