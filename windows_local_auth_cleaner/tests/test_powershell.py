from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock

from browser_cleaner.developer_auth import DeveloperAuthService
from browser_cleaner.powershell import PowerShellProfileManager


class PowerShellProfileTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.documents = Path(self.directory.name).resolve()
        self.manager = PowerShellProfileManager(self.documents)
        self.block = self.manager.begin + "\r\nfunction global:mac { ssh example }\r\n" + self.manager.end + "\r\n"

    def write_profile(self, content, encoding="utf-8-sig"):
        path = self.manager.paths()[0]
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(content.encode(encoding))
        return path

    def test_dry_run_and_decline_preserve_profile(self):
        path = self.write_profile(self.block)
        original = path.read_bytes()
        self.assertEqual(self.manager.audit()[0].login_status, "present")
        self.assertEqual(self.manager.remove(False, lambda *_: self.fail())[0].status, "dry-run")
        self.assertEqual(self.manager.remove(True, lambda *_: False)[0].status, "manual")
        self.assertEqual(path.read_bytes(), original)

    def test_owned_profiles_are_deleted_in_both_editions_and_verified(self):
        for path in self.manager.paths():
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(self.block.encode("utf-8-sig"))
        self.assertTrue(all(entry.status == "manual verification required" for entry in self.manager.verify()))
        self.assertEqual([r.status for r in self.manager.remove(True, lambda *_: True)], ["removed", "removed"])
        self.assertTrue(all(not path.exists() for path in self.manager.paths()))
        self.assertEqual(self.manager.remove(True, lambda *_: self.fail()), [])
        self.assertTrue(all(entry.status == "no local evidence found" for entry in self.manager.verify()))

    def test_existing_content_and_encoding_survive(self):
        for encoding in ("utf-8", "utf-8-sig", "utf-16"):
            with self.subTest(encoding=encoding):
                before = "# 사용자 설정\r\n$env:EDITOR = 'nvim'\r\n"
                after = "function personal { 'hello' }\r\n"
                path = self.write_profile(before + self.block + after, encoding)
                self.assertEqual(self.manager.remove(True, lambda *_: True)[0].status, "removed")
                self.assertEqual(path.read_bytes(), (before + after).encode(encoding))

    def test_unmanaged_and_malformed_profiles_are_preserved(self):
        for content, expected in (("function mac { 'personal' }", []),
                                  (self.manager.begin + "\nsecret-value", ["error"]),
                                  (self.block * 2, ["error"])):
            path = self.write_profile(content)
            original = path.read_bytes()
            results = self.manager.remove(True, lambda *_: True)
            self.assertEqual([entry.status for entry in results], expected)
            self.assertNotIn("secret-value", str(results))
            self.assertEqual(path.read_bytes(), original)

    def test_symlink_and_linked_parent_are_refused(self):
        outside = self.documents / "outside.ps1"
        outside.write_bytes(self.block.encode())
        path = self.manager.paths()[0]
        path.parent.mkdir()
        try:
            path.symlink_to(outside)
        except OSError:
            self.skipTest("symlink creation unavailable")
        self.assertEqual(self.manager.remove(True, lambda *_: True)[0].status, "error")
        path.unlink()
        path.parent.rmdir()
        outside_dir = self.documents / "outside"
        outside_dir.mkdir()
        (outside_dir / path.name).write_bytes(self.block.encode())
        path.parent.symlink_to(outside_dir, target_is_directory=True)
        self.assertEqual(self.manager.remove(True, lambda *_: True)[0].status, "error")
        self.assertEqual(outside.read_bytes(), self.block.encode())
        self.assertTrue((outside_dir / path.name).exists())

    def test_developer_auth_cleanup_includes_managed_profiles(self):
        runner = Mock()
        runner.run.return_value = None
        store = Mock()
        store.list_supported_entries.return_value = []
        store.list_token_names.return_value = set()
        service = DeveloperAuthService(runner=runner, credential_store=store,
                                       environment_store=store, code_commands=(),
                                       devtunnel_command="missing-devtunnel",
                                       powershell_profiles=self.manager)
        path = self.write_profile(self.block)
        results = service.remove_supported(True, lambda *_: True)
        self.assertTrue(any(r.provider == "PowerShell profile" and r.status == "removed" for r in results))
        self.assertFalse(path.exists())


if __name__ == "__main__":
    unittest.main()
