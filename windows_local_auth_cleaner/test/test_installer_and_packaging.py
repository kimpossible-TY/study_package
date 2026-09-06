"""Tests for installation scripts, Scoop manifest, and packaging configurations."""

from __future__ import annotations

import json
from pathlib import Path
import unittest


class PackagingAndInstallerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.project_root = Path(__file__).resolve().parent.parent
        self.cleaner_dir = self.project_root

    def test_scoop_manifest_is_valid_json_and_configured_for_release(self) -> None:
        manifest_path = self.cleaner_dir / "windows-local-auth-cleaner.json"
        self.assertTrue(manifest_path.exists(), "windows-local-auth-cleaner.json must exist")

        with manifest_path.open("r", encoding="utf-8") as f:
            data = json.load(f)

        self.assertIn("version", data)
        self.assertIn("architecture", data)
        self.assertIn("64bit", data["architecture"])
        arch_64 = data["architecture"]["64bit"]
        self.assertIn("url", arch_64)
        self.assertIn("releases", arch_64["url"])
        self.assertIn("windows-local-auth-cleaner.exe", arch_64["url"])
        self.assertIn("bin", arch_64)
        self.assertTrue(any("wac" in item for item in arch_64["bin"]))

    def test_install_ps1_contains_required_parameters_and_repo(self) -> None:
        install_script = self.cleaner_dir / "install.ps1"
        self.assertTrue(install_script.exists(), "install.ps1 must exist")

        content = install_script.read_text(encoding="utf-8")
        self.assertIn("param(", content)
        self.assertIn("$Tag", content)
        self.assertIn("$InstallDir", content)
        self.assertIn("$Uninstall", content)
        self.assertIn("kimpossible-TY/study_package", content)
        self.assertIn("windows-local-auth-cleaner.exe", content)
        self.assertIn("wac.cmd", content)

    def test_github_workflow_includes_release_publishing(self) -> None:
        repo_root = self.project_root.parent
        workflow_path = repo_root / ".github" / "workflows" / "windows-local-auth-cleaner.yml"
        self.assertTrue(workflow_path.exists(), "windows-local-auth-cleaner.yml must exist")

        content = workflow_path.read_text(encoding="utf-8")
        self.assertIn("gh release upload", content)
        self.assertIn("gh release create", content)
        self.assertIn("windows-local-auth-cleaner.exe", content)


if __name__ == "__main__":
    unittest.main()
