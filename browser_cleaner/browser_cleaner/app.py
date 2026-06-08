from __future__ import annotations

import os

from .cleaning import PathCleaner
from .cli import ArgumentParserFactory, CleanerOptionsFactory
from .config import BrowserCatalog
from .models import CleanResult, CleanerOptions
from .processes import BrowserProcessManager
from .profiles import ProfileFinder
from .tui import TerminalUI


class BrowserCleanerApp:
    def __init__(
        self,
        options: CleanerOptions,
        catalog: BrowserCatalog,
        profile_finder: ProfileFinder,
        process_manager: BrowserProcessManager,
        path_cleaner: PathCleaner,
        ui: TerminalUI,
    ) -> None:
        self._options = options
        self._catalog = catalog
        self._profile_finder = profile_finder
        self._process_manager = process_manager
        self._path_cleaner = path_cleaner
        self._ui = ui

    @classmethod
    def from_command_line(cls) -> BrowserCleanerApp:
        catalog = BrowserCatalog()
        options = CleanerOptionsFactory(ArgumentParserFactory()).from_args(catalog.keys())
        return cls(
            options=options,
            catalog=catalog,
            profile_finder=ProfileFinder(),
            process_manager=BrowserProcessManager(),
            path_cleaner=PathCleaner(),
            ui=TerminalUI(),
        )

    def run(self) -> int:
        self._ui.setup()
        self._ui.header(
            "Windows Browser Privacy Cleaner",
            "history | search | login records | cookies | cache | sessions",
        )

        if os.name != "nt":
            self._ui.warning("This cleaner is designed for Windows paths. Dry-run discovery may not match this OS.")

        if self._options.apply_changes and not self._ui.confirm_apply(self._options.skip_confirmation):
            self._ui.error("Confirmation failed. Nothing was deleted.")
            return 1

        results = self._clean_selected_browsers()
        self._ui.print_results(results)
        self._ui.print_summary(results, self._options.apply_changes)
        self._ui.success("Finished.")
        return 0

    def _clean_selected_browsers(self) -> list[CleanResult]:
        results: list[CleanResult] = []
        for browser in self._catalog.select(self._options.browser_key):
            self._ui.step(f"Checking {browser.display_name}")
            self._handle_running_browser(browser.display_name, browser.process_names)

            profiles = self._profile_finder.find_profiles(browser)
            if not profiles:
                self._ui.warning(f"No profiles found for {browser.display_name}.")
                continue

            for profile in profiles:
                self._ui.step(f"Scanning {browser.display_name} profile: {profile.name}")
                for target in self._profile_finder.find_targets(browser, profile):
                    results.append(self._path_cleaner.clean(target, self._options.apply_changes))
        return results

    def _handle_running_browser(self, browser_name: str, process_names: tuple[str, ...]) -> None:
        running = self._process_manager.running_processes(process_names)
        if not running:
            return

        if self._options.close_browsers:
            self._ui.warning(f"{browser_name} is running. Closing: {', '.join(sorted(running))}")
            self._process_manager.close_processes(process_names)
            return

        self._ui.warning(f"{browser_name} is running. Some files may be locked. Use --kill to close it.")
