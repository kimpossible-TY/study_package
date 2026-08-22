"""Top-level safe orchestration for browser and developer-auth cleanup."""

from __future__ import annotations

import os
from pathlib import Path

from .cleaning import PathCleaner
from .cli import ArgumentParserFactory, CleanerOptionsFactory
from .config import BrowserCatalog
from .developer_auth import DeveloperAuthService
from .models import BROWSER_CATEGORIES, AccountAuditEntry, ActionResult, CleanResult, CleanTarget, CleanerOptions
from .processes import BrowserProcessManager
from .profiles import ProfileFinder
from .tui import TerminalUI
from .verification import VerificationService
from .vscode import VscodeProfileManager


class BrowserCleanerApp:
    """Orchestrate discovery, explicit confirmation, removal, and verification.

    Browser privacy artifacts and developer-account artifacts have deliberately
    separate execution paths. The latter always retains per-record strong
    confirmations and never relies on ``--yes``.
    """

    def __init__(
        self,
        options: CleanerOptions,
        catalog: BrowserCatalog,
        profile_finder: ProfileFinder,
        process_manager: BrowserProcessManager,
        path_cleaner: PathCleaner,
        ui: TerminalUI,
        developer_auth: DeveloperAuthService | None = None,
        vscode: VscodeProfileManager | None = None,
        verification: VerificationService | None = None,
    ) -> None:
        self._options = options
        self._catalog = catalog
        self._profile_finder = profile_finder
        self._process_manager = process_manager
        self._path_cleaner = path_cleaner
        self._ui = ui
        self._developer_auth = developer_auth or DeveloperAuthService()
        self._vscode = vscode or VscodeProfileManager(cleaner=path_cleaner, process_manager=process_manager)
        self._verification = verification or VerificationService(
            self._developer_auth, catalog, profile_finder, self._vscode
        )

    @classmethod
    def from_command_line(cls) -> "BrowserCleanerApp":
        """Construct the application with Windows-aware default services."""
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
            "Windows Local Authentication Cleaner",
            "dry run by default | browser privacy data | developer authentication",
        )
        if os.name != "nt":
            self._ui.warning("This cleaner targets Windows. No Windows destructive action will run on this host.")

        if "full-profile" in self._options.categories and not self._options.browser_profile_paths:
            self._ui.error("--category full-profile needs one or more --wipe-browser-profile PROFILE_PATH values.")
            return 2

        wants_developer_auth = "developer-auth" in self._options.categories or self._is_unified_default_mode()
        if self._options.audit_accounts or wants_developer_auth:
            self._ui.print_audit(self._account_audit())
            self._ui.print_vscode_manual_guidance()

        # An audit by itself has no hidden cleanup side effect.
        if self._options.audit_accounts and not self._has_requested_mutation_or_backup() and not self._options.verify:
            self._ui.print_remote_revocation_guidance()
            return 0

        action_results: list[ActionResult] = []
        if self._options.vscode_backup is not None:
            action_results.extend(self._vscode.backup(self._options.vscode_backup))

        browser_categories = self._browser_cleanup_categories()
        browser_targets = self._discover_selected_browser_targets(browser_categories) if browser_categories else []
        if browser_categories and self._apply_changes:
            self._ui.print_targets(browser_targets)
        if browser_categories and self._apply_changes and not self._ui.confirm_apply(self._options.skip_confirmation):
            self._ui.error("Confirmation failed. Nothing was deleted.")
            return 1

        if browser_categories:
            self._handle_running_selected_browsers()
        browser_results = [self._path_cleaner.clean(target, self._apply_changes) for target in browser_targets]
        if self._options.browser_profile_paths and not self._options.verify:
            self._ui.warning("Browser full-profile wipe removes all local data in the exact selected profile. Sync should be off first.")
            action_results.extend(self._wipe_browser_profiles())
        if self._options.wipe_vscode_profile and not self._options.verify:
            self._ui.warning("VS Code profile wipe can remove settings, keybindings, snippets, workspace and extension state, and local chat/session data.")
            action_results.extend(self._vscode.wipe(self._apply_changes, self._ui.confirm_sensitive))
        if wants_developer_auth and not self._options.verify:
            action_results.extend(self._developer_auth.remove_supported(self._apply_changes, self._ui.confirm_sensitive))

        self._ui.print_results(browser_results)
        if browser_categories:
            self._ui.print_summary(browser_results, self._apply_changes)
        self._ui.print_action_results(action_results)

        if self._options.verify or self._apply_changes and (browser_categories or wants_developer_auth or self._options.wipe_vscode_profile or self._options.browser_profile_paths):
            verification = self._verification.verify(self._options, include_developer_auth=wants_developer_auth)
            self._ui.print_verification(verification)

        if browser_categories or self._options.browser_profile_paths:
            self._ui.print_browser_sync_guidance()
        if wants_developer_auth or self._options.audit_accounts:
            self._ui.print_remote_revocation_guidance()
        if self._options.wipe_vscode_profile or self._options.browser_profile_paths:
            self._ui.print_windows_profile_guidance()
        self._ui.success("Finished.")
        return 0

    def _discover_selected_browser_targets(self, categories: tuple[str, ...]) -> list[CleanTarget]:
        targets: list[CleanTarget] = []
        for browser in self._catalog.select(self._options.browser_key):
            self._ui.step(f"Checking {browser.display_name}")
            profiles = self._profile_finder.find_profiles(browser)
            if not profiles:
                self._ui.warning(f"No safe profiles found for {browser.display_name}.")
                continue
            for profile in profiles:
                self._ui.step(f"Scanning {browser.display_name} profile: {profile.name}")
                targets.extend(self._profile_finder.find_targets(browser, profile, categories))
        return targets

    def _account_audit(self) -> list[AccountAuditEntry]:
        """Add browser sync evidence without reading private browser state files."""
        entries = self._developer_auth.audit()
        sync_surfaces = {
            "chrome": "Google Chrome sync account (not inspected)",
            "edge": "Microsoft Edge sync account (not inspected)",
            "firefox": "Firefox Sync account (not inspected)",
            "brave": "Brave sync/account (not inspected)",
        }
        for browser in self._catalog.all():
            profiles = self._profile_finder.find_profiles(browser)
            if profiles:
                for profile in profiles:
                    entries.append(
                        AccountAuditEntry(
                            browser.display_name,
                            sync_surfaces.get(browser.key, "Browser sync/account (not inspected)"),
                            str(profile),
                            "manual verification required",
                            "no",
                            "yes",
                        )
                    )
            else:
                entries.append(
                    AccountAuditEntry(
                        browser.display_name,
                        "local profile not found",
                        "; ".join(str(root) for root in browser.profile_roots) or "current-user browser root unavailable",
                        "no local evidence found",
                        "no",
                        "yes",
                    )
                )
        return entries

    def _handle_running_selected_browsers(self) -> None:
        for browser in self._catalog.select(self._options.browser_key):
            self._handle_running_browser(browser.display_name, browser.process_names)

    def _wipe_browser_profiles(self) -> list[ActionResult]:
        discovered: dict[str, tuple[str, Path, tuple[str, ...]]] = {}
        for browser in self._catalog.select(self._options.browser_key):
            for profile in self._profile_finder.find_profiles(browser):
                discovered[self._path_key(profile)] = (browser.display_name, profile, browser.process_names)

        results: list[ActionResult] = []
        for requested in self._options.browser_profile_paths:
            matched = discovered.get(self._path_key(requested))
            if matched is None:
                results.append(
                    ActionResult("Browser profile", requested.name or str(requested), str(requested), "full-profile", "error", "path is not an exact discovered profile under a supported current-user browser root")
                )
                continue
            browser_name, profile, process_names = matched
            running = self._process_manager.running_processes(process_names)
            if running:
                results.append(ActionResult(browser_name, profile.name, str(profile), "full-profile", "locked", "browser is running; close it normally before a full-profile wipe"))
                continue
            if not self._apply_changes:
                results.append(ActionResult(browser_name, profile.name, str(profile), "full-profile", "dry-run", "would remove the exact complete local browser profile"))
                continue
            phrase = f"WIPE BROWSER PROFILE {profile}"
            if not self._ui.confirm_sensitive(f"{browser_name} profile '{profile}'", phrase):
                results.append(ActionResult(browser_name, profile.name, str(profile), "full-profile", "manual", "strong confirmation declined"))
                continue
            clean_result = self._path_cleaner.clean(CleanTarget(browser_name, profile.name, profile, "full-profile", profile), True)
            results.append(ActionResult(browser_name, profile.name, str(profile), "full-profile", clean_result.status, clean_result.details))
        return results

    def _handle_running_browser(self, browser_name: str, process_names: tuple[str, ...]) -> None:
        running = self._process_manager.running_processes(process_names)
        if not running:
            return
        if self._options.close_browsers:
            if not self._apply_changes:
                self._ui.warning(f"{browser_name} is running. Dry run will not force-close it; --kill only acts with --apply.")
                return
            self._ui.warning(f"{browser_name} is running. --kill force-closes: {', '.join(sorted(running))}")
            self._process_manager.close_processes(process_names)
            return
        self._ui.warning(f"{browser_name} is running. Close it normally; otherwise some files may be locked. --kill is forceful.")

    def _browser_cleanup_categories(self) -> tuple[str, ...]:
        if self._options.verify:
            return ()
        if self._options.categories:
            return tuple(category for category in self._options.categories if category in BROWSER_CATEGORIES)
        if self._options.audit_accounts or self._options.verify or self._options.wipe_vscode_profile or self._options.vscode_backup or self._options.browser_profile_paths:
            return ()
        # Preserve existing no-argument behavior, with the important distinction
        # that it remains a dry run until --apply is supplied.
        return BROWSER_CATEGORIES

    def _is_unified_default_mode(self) -> bool:
        """Whether the two-command review/apply experience is active.

        Advanced selectors retain their narrowly scoped legacy behavior.  A
        plain invocation (or plain ``--apply``) reviews/removes normal browser
        data plus supported developer-authentication candidates.
        """
        return not (
            self._options.categories
            or self._options.audit_accounts
            or self._options.verify
            or self._options.wipe_vscode_profile
            or self._options.vscode_backup
            or self._options.browser_profile_paths
            or self._options.browser_key != "all"
        )

    @property
    def _apply_changes(self) -> bool:
        """Never perform Windows-oriented destructive work from a non-Windows host."""
        return self._options.apply_changes and os.name == "nt"

    def _has_requested_mutation_or_backup(self) -> bool:
        return bool(
            self._options.categories
            or self._options.wipe_vscode_profile
            or self._options.vscode_backup
            or self._options.browser_profile_paths
        )

    @staticmethod
    def _path_key(path: Path) -> str:
        return os.path.normcase(os.path.abspath(os.path.normpath(os.fspath(path))))
