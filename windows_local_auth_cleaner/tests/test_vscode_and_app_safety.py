from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

from browser_cleaner.app import BrowserCleanerApp
from browser_cleaner.cleaning import PathCleaner
from browser_cleaner.config import BrowserCatalog
from browser_cleaner.models import BrowserDefinition, CleanerOptions
from browser_cleaner.profiles import ProfileFinder
from browser_cleaner.tui import TerminalUI
from browser_cleaner.vscode import VscodeProfileManager


class NoRunningProcesses:
    def running_processes(self, _names):
        return set()

    def close_processes(self, _names):
        raise AssertionError("force-close should not be used in this test")


class RecordingUI(TerminalUI):
    def __init__(self) -> None:
        super().__init__()
        self.events: list[str] = []

    def setup(self):
        pass

    def header(self, _title, _subtitle):
        pass

    def step(self, _message):
        pass

    def warning(self, _message):
        pass

    def success(self, _message):
        pass

    def print_targets(self, _targets):
        self.events.append("targets")

    def confirm_apply(self, _skip):
        self.events.append("confirm")
        return True

    def print_results(self, _results):
        self.events.append("results")

    def print_summary(self, _results, _apply):
        pass

    def print_browser_sync_guidance(self):
        pass

    def print_verification(self, _entries):
        pass


class TemporaryWindowsApplyApp(BrowserCleanerApp):
    """Exercise the live orchestration path without changing the host OS mode."""

    @property
    def _apply_changes(self):
        return self._options.apply_changes


class FakeDeveloperAuth:
    def __init__(self) -> None:
        self.remove_apply_values: list[bool] = []

    def audit(self):
        return []

    def remove_supported(self, apply_changes, _confirm):
        self.remove_apply_values.append(apply_changes)
        return []


class NoopVerification:
    def verify(self, _options, include_developer_auth=False):
        return []


class VscodeAndAppSafetyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def _manager(self) -> VscodeProfileManager:
        app_data = self.root / "AppData"
        user_profile = self.root / "User"
        user = app_data / "Code" / "User"
        (user / "snippets").mkdir(parents=True)
        (user / "settings.json").write_text('{"editor.fontSize": 14}', encoding="utf-8")
        (user / "keybindings.json").write_text("[]", encoding="utf-8")
        (user / "snippets" / "python.json").write_text("{}", encoding="utf-8")
        (user / "globalStorage" / "auth").mkdir(parents=True)
        (user / "globalStorage" / "auth" / "do-not-backup").write_text("fixture", encoding="utf-8")
        (user_profile / ".vscode" / "extensions" / "github.copilot-1.0.0").mkdir(parents=True)
        return VscodeProfileManager(
            app_data=app_data,
            user_profile=user_profile,
            cleaner=PathCleaner(),
            process_manager=NoRunningProcesses(),
        )

    def test_safe_backup_excludes_auth_state_and_wipe_requires_confirmation(self) -> None:
        manager = self._manager()
        backup = self.root / "safe-backup"

        backup_result = manager.backup(backup)

        self.assertEqual(backup_result[0].status, "backed-up")
        self.assertTrue((backup / "vs-code-stable" / "settings.json").exists())
        self.assertTrue((backup / "vs-code-stable" / "keybindings.json").exists())
        self.assertTrue((backup / "vs-code-stable" / "snippets" / "python.json").exists())
        self.assertFalse((backup / "vs-code-stable" / "globalStorage").exists())
        self.assertEqual((backup / "extensions-stable.txt").read_text(encoding="utf-8"), "github.copilot-1.0.0\n")

        dry_run = manager.wipe(False, lambda _subject, _phrase: True)
        self.assertEqual(dry_run[0].status, "dry-run")
        code_root = self.root / "AppData" / "Code"
        self.assertTrue(code_root.exists())
        declined = manager.wipe(True, lambda _subject, _phrase: False)
        self.assertEqual(declined[0].status, "manual")
        self.assertTrue(code_root.exists())
        removed = manager.wipe(True, lambda _subject, _phrase: True)
        self.assertEqual(removed[0].status, "removed")
        self.assertFalse(code_root.exists())

    @unittest.skipIf(os.name == "nt", "this test asserts the non-Windows application guard")
    def test_app_apply_is_dry_run_on_non_windows(self) -> None:
        profile_root = self.root / "profiles"
        profile = profile_root / "Default"
        profile.mkdir(parents=True)
        history = profile / "History"
        history.write_text("fixture", encoding="utf-8")
        catalog = BrowserCatalog()
        catalog._browsers = [
            BrowserDefinition("fixture", "Fixture Browser", (), (profile_root,), (), (), "chromium")
        ]
        options = CleanerOptions(
            apply_changes=True,
            skip_confirmation=True,
            close_browsers=False,
            browser_key="fixture",
            categories=("history",),
        )
        app = BrowserCleanerApp(
            options, catalog, ProfileFinder(), NoRunningProcesses(), PathCleaner(), TerminalUI()
        )

        self.assertEqual(app.run(), 0)
        self.assertTrue(history.exists())

    def test_plain_invocation_is_the_unified_review_command(self) -> None:
        profile_root = self.root / "profiles"
        profile = profile_root / "Default"
        profile.mkdir(parents=True)
        (profile / "History").write_text("fixture", encoding="utf-8")
        catalog = BrowserCatalog()
        catalog._browsers = [
            BrowserDefinition("fixture", "Fixture Browser", (), (profile_root,), (), (), "chromium")
        ]
        developer_auth = FakeDeveloperAuth()
        app = BrowserCleanerApp(
            CleanerOptions(False, False, False, "all"),
            catalog,
            ProfileFinder(),
            NoRunningProcesses(),
            PathCleaner(),
            RecordingUI(),
            developer_auth=developer_auth,
            verification=NoopVerification(),
        )

        self.assertEqual(app.run(), 0)
        self.assertEqual(developer_auth.remove_apply_values, [False])

    @unittest.skipIf(os.name == "nt", "the temporary Windows-mode simulation is only needed on non-Windows")
    def test_apply_shows_exact_targets_before_confirmation(self) -> None:
        profile_root = self.root / "profiles"
        profile = profile_root / "Default"
        profile.mkdir(parents=True)
        history = profile / "History"
        history.write_text("fixture", encoding="utf-8")
        catalog = BrowserCatalog()
        catalog._browsers = [
            BrowserDefinition("fixture", "Fixture Browser", (), (profile_root,), (), (), "chromium")
        ]
        ui = RecordingUI()
        options = CleanerOptions(True, False, False, "fixture", categories=("history",))
        app = TemporaryWindowsApplyApp(options, catalog, ProfileFinder(), NoRunningProcesses(), PathCleaner(), ui)

        self.assertEqual(app.run(), 0)

        self.assertEqual(ui.events, ["targets", "confirm", "results"])
        self.assertFalse(history.exists())


if __name__ == "__main__":
    unittest.main()
