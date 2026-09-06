"""Tests for one-shot execution and automatic browser closing."""

from __future__ import annotations

import unittest
from unittest.mock import MagicMock, patch

from browser_cleaner.app import BrowserCleanerApp
from browser_cleaner.cleaning import PathCleaner
from browser_cleaner.cli import ArgumentParserFactory, CleanerOptionsFactory
from browser_cleaner.config import BrowserCatalog
from browser_cleaner.models import BrowserDefinition, CleanerOptions
from browser_cleaner.processes import BrowserProcessManager
from browser_cleaner.profiles import ProfileFinder
from browser_cleaner.tui import TerminalUI


class MockRunningProcesses(BrowserProcessManager):
    def __init__(self, running: set[str] | None = None) -> None:
        self.currently_running = set(running or set())
        self.closed_calls: list[list[str]] = []

    def running_processes(self, process_names):
        return {name.lower() for name in process_names if name.lower() in self.currently_running}

    def close_processes(self, process_names, graceful=True, timeout=2.0):
        self.closed_calls.append(list(process_names))
        for name in process_names:
            self.currently_running.discard(name.lower())


class MockUI(TerminalUI):
    def __init__(self) -> None:
        super().__init__()
        self.warnings: list[str] = []
        self.confirmed_sensitive: list[tuple[str, str]] = []

    def setup(self):
        pass

    def header(self, _title, _subtitle):
        pass

    def warning(self, message: str):
        self.warnings.append(message)

    def success(self, _message):
        pass

    def confirm_sensitive(self, subject: str, phrase: str, skip_prompt: bool = False) -> bool:
        self.confirmed_sensitive.append((subject, phrase))
        return True


class OneShotAndBrowserClosingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.catalog = BrowserCatalog()
        self.catalog._browsers = [
            BrowserDefinition("chrome", "Google Chrome", ("chrome.exe",), (), (), (), "chromium")
        ]

    def test_default_cli_args_produce_one_shot_options(self) -> None:
        factory = CleanerOptionsFactory(ArgumentParserFactory())
        options = factory.from_args(("chrome",), [])

        self.assertTrue(options.apply_changes)
        self.assertTrue(options.skip_confirmation)
        self.assertTrue(options.close_browsers)
        self.assertTrue(options.one_shot)
        self.assertFalse(options.interactive)

    def test_dry_run_flag_disables_apply_and_close(self) -> None:
        factory = CleanerOptionsFactory(ArgumentParserFactory())
        options = factory.from_args(("chrome",), ["--dry-run"])

        self.assertFalse(options.apply_changes)
        self.assertFalse(options.skip_confirmation)
        self.assertFalse(options.close_browsers)
        self.assertFalse(options.one_shot)

    def test_interactive_flag_launches_tui_mode(self) -> None:
        factory = CleanerOptionsFactory(ArgumentParserFactory())
        options = factory.from_args(("chrome",), ["--interactive"])

        self.assertTrue(options.interactive)
        self.assertFalse(options.apply_changes)
        self.assertFalse(options.one_shot)

    def test_prompt_and_keep_browsers_flags(self) -> None:
        factory = CleanerOptionsFactory(ArgumentParserFactory())
        options = factory.from_args(("chrome",), ["--prompt", "--keep-browsers"])

        self.assertTrue(options.apply_changes)
        self.assertFalse(options.skip_confirmation)
        self.assertFalse(options.close_browsers)

    def test_running_browsers_automatically_closed_when_applying(self) -> None:
        proc_manager = MockRunningProcesses(running={"chrome.exe"})
        ui = MockUI()
        options = CleanerOptions(
            apply_changes=True,
            skip_confirmation=True,
            close_browsers=True,
            browser_key="chrome",
            categories=("history",),
        )

        app = BrowserCleanerApp(
            options,
            self.catalog,
            ProfileFinder(),
            proc_manager,
            PathCleaner(),
            ui,
        )

        with patch.object(BrowserCleanerApp, "_apply_changes", True):
            app._handle_running_browser("Google Chrome", ("chrome.exe",))

        self.assertEqual(proc_manager.closed_calls, [[("chrome.exe",)[0]]])
        self.assertIn("chrome.exe", proc_manager.closed_calls[0])
        self.assertTrue(any("Automatically closing" in w for w in ui.warnings))

    def test_running_browsers_warned_without_closing_in_dry_run(self) -> None:
        proc_manager = MockRunningProcesses(running={"chrome.exe"})
        ui = MockUI()
        options = CleanerOptions(
            apply_changes=False,
            skip_confirmation=False,
            close_browsers=False,
            browser_key="chrome",
            categories=("history",),
        )

        app = BrowserCleanerApp(
            options,
            self.catalog,
            ProfileFinder(),
            proc_manager,
            PathCleaner(),
            ui,
        )

        app._handle_running_browser("Google Chrome", ("chrome.exe",))

        self.assertEqual(proc_manager.closed_calls, [])
        self.assertTrue(any("Dry run will not close it" in w for w in ui.warnings))

    def test_sensitive_confirmation_skipped_when_skip_confirmation_is_true(self) -> None:
        ui = MockUI()
        options = CleanerOptions(
            apply_changes=True,
            skip_confirmation=True,
            close_browsers=True,
            browser_key="all",
        )

        app = BrowserCleanerApp(
            options,
            self.catalog,
            ProfileFinder(),
            MockRunningProcesses(),
            PathCleaner(),
            ui,
        )

        # Calling _confirm_sensitive should return True directly without calling ui.confirm_sensitive
        result = app._confirm_sensitive("GitHub CLI account octo", "REMOVE GH octo")
        self.assertTrue(result)
        self.assertEqual(ui.confirmed_sensitive, [])

    def test_sensitive_confirmation_prompted_when_skip_confirmation_is_false(self) -> None:
        ui = MockUI()
        options = CleanerOptions(
            apply_changes=True,
            skip_confirmation=False,
            close_browsers=True,
            browser_key="all",
        )

        app = BrowserCleanerApp(
            options,
            self.catalog,
            ProfileFinder(),
            MockRunningProcesses(),
            PathCleaner(),
            ui,
        )

        result = app._confirm_sensitive("GitHub CLI account octo", "REMOVE GH octo")
        self.assertTrue(result)
        self.assertEqual(len(ui.confirmed_sensitive), 1)
        self.assertEqual(ui.confirmed_sensitive[0][1], "REMOVE GH octo")

    def test_browser_process_manager_close_processes_graceful_and_force(self) -> None:
        manager = BrowserProcessManager()
        with patch("subprocess.run") as mock_run:
            # Simulate tasklist returning chrome.exe first, then empty after taskkill
            with patch.object(manager, "running_processes", side_effect=[{"chrome.exe"}, set()]):
                manager.close_processes(["chrome.exe"], graceful=True, timeout=0.5)

            # taskkill without /f should have been called first
            mock_run.assert_called_with(
                ["taskkill", "/im", "chrome.exe"],
                capture_output=True,
                text=True,
                check=False,
            )


if __name__ == "__main__":
    unittest.main()
