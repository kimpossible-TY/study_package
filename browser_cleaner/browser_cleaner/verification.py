"""Conservative, non-destructive local verification checks."""

from __future__ import annotations

from .config import BrowserCatalog
from .developer_auth import DeveloperAuthService
from .models import BROWSER_CATEGORIES, CleanerOptions, VerificationEntry
from .profiles import ProfileFinder
from .vscode import VscodeProfileManager


class VerificationService:
    """Report evidence, not an overconfident claim that an account is gone everywhere."""

    def __init__(
        self,
        developer_auth: DeveloperAuthService,
        catalog: BrowserCatalog,
        profile_finder: ProfileFinder,
        vscode: VscodeProfileManager,
    ) -> None:
        self._developer_auth = developer_auth
        self._catalog = catalog
        self._profile_finder = profile_finder
        self._vscode = vscode

    def verify(self, options: CleanerOptions, include_developer_auth: bool = False) -> list[VerificationEntry]:
        entries: list[VerificationEntry] = []
        categories = self._browser_categories(options)
        if categories:
            entries.extend(self._verify_browsers(options, categories))
        if options.browser_profile_paths:
            entries.extend(self._verify_full_browser_profiles(options))
        if include_developer_auth or options.verify and not options.categories:
            entries.extend(self._verify_developer_auth())
            entries.extend(self._verify_vscode())
        elif "developer-auth" in options.categories:
            entries.extend(self._verify_developer_auth())
        if options.wipe_vscode_profile:
            entries.extend(self._verify_vscode())
        if include_developer_auth or options.verify:
            entries.append(
                VerificationEntry(
                    "Remote tokens and cloud sessions",
                    "GitHub, Microsoft, and Google account security pages",
                    "remote revocation recommended",
                    "local logout/removal does not revoke remote OAuth tokens, PATs, SSH keys, or cloud sessions",
                )
            )
        return entries

    def _verify_browsers(self, options: CleanerOptions, categories: tuple[str, ...]) -> list[VerificationEntry]:
        entries: list[VerificationEntry] = []
        for browser in self._catalog.select(options.browser_key):
            profiles = self._profile_finder.find_profiles(browser)
            if not profiles:
                entries.append(VerificationEntry(browser.display_name, "browser profile roots", "no local evidence found", "no recognized local browser profile was found"))
                continue
            for profile in profiles:
                targets = self._profile_finder.find_targets(browser, profile, categories)
                if targets:
                    entries.append(VerificationEntry(f"{browser.display_name} / {profile.name}", str(profile), "manual verification required", "selected local files still exist or were recreated; check browser sign-in and sync in its UI"))
                else:
                    entries.append(VerificationEntry(f"{browser.display_name} / {profile.name}", str(profile), "no local evidence found", "no selected local browser artifacts were found"))
        return entries

    @staticmethod
    def _verify_full_browser_profiles(options: CleanerOptions) -> list[VerificationEntry]:
        entries: list[VerificationEntry] = []
        for path in options.browser_profile_paths:
            status = "manual verification required" if path.exists() else "no local evidence found"
            details = "profile path still exists; check browser sign-in and sync in its UI" if path.exists() else "the selected profile path is absent"
            entries.append(VerificationEntry("Browser full profile", str(path), status, details))
        return entries

    def _verify_developer_auth(self) -> list[VerificationEntry]:
        entries: list[VerificationEntry] = []
        gh_accounts = self._developer_auth.github_cli_accounts()
        if gh_accounts is None:
            entries.append(VerificationEntry("GitHub CLI", "gh auth status", "manual verification required", "GitHub CLI is unavailable"))
        elif gh_accounts:
            entries.append(VerificationEntry("GitHub CLI", "gh auth status", "manual verification required", "one or more local GitHub CLI accounts remain"))
        else:
            entries.append(VerificationEntry("GitHub CLI", "gh auth status", "no local evidence found", "no logged-in GitHub CLI account was reported"))

        gcm_accounts = self._developer_auth.git_credential_manager_accounts()
        if gcm_accounts is None:
            entries.append(VerificationEntry("Git Credential Manager", "git credential-manager github list", "manual verification required", "Git Credential Manager is unavailable"))
        elif gcm_accounts:
            entries.append(VerificationEntry("Git Credential Manager", "git credential-manager github list", "manual verification required", "one or more listed GitHub accounts remain"))
        else:
            entries.append(VerificationEntry("Git Credential Manager", "git credential-manager github list", "no local evidence found", "no listed GitHub account was found"))

        credentials = self._developer_auth.credential_entries()
        if credentials is None:
            entries.append(VerificationEntry("Windows Credential Manager", "current-user supported targets", "manual verification required", "Windows Credential API is unavailable"))
        elif credentials:
            entries.append(VerificationEntry("Windows Credential Manager", "current-user supported targets", "manual verification required", "supported developer credential targets remain"))
        else:
            entries.append(VerificationEntry("Windows Credential Manager", "current-user supported targets", "no local evidence found", "no supported developer credential target was found"))

        names = self._developer_auth.environment_token_names()
        if names is None:
            entries.append(VerificationEntry("Current-user environment", "HKCU\\Environment", "manual verification required", "current-user registry environment is unavailable"))
        elif names:
            entries.append(VerificationEntry("Current-user environment", "HKCU\\Environment", "manual verification required", "allowlisted token variable names remain"))
        else:
            entries.append(VerificationEntry("Current-user environment", "HKCU\\Environment", "no local evidence found", "no allowlisted current-user token variable was found"))

        for command in self._developer_auth.code_commands:
            state = self._developer_auth.tunnel_status(command)
            status = "no local evidence found" if state == "not registered" else "manual verification required"
            details = "no registered tunnel was reported" if state == "not registered" else f"{state}; confirm in VS Code Remote Tunnels UI"
            entries.append(VerificationEntry("VS Code Remote Tunnel", f"{command} tunnel status", status, details))
        return entries

    def _verify_vscode(self) -> list[VerificationEntry]:
        entries: list[VerificationEntry] = []
        for display_name, root in self._vscode.roots():
            status = "manual verification required" if root.exists() else "no local evidence found"
            details = "use the Accounts menu and Settings Sync UI; this tool does not inspect auth databases" if root.exists() else "local VS Code user-data profile is absent"
            entries.append(VerificationEntry(display_name, str(root), status, details))
        return entries

    @staticmethod
    def _browser_categories(options: CleanerOptions) -> tuple[str, ...]:
        if options.categories:
            return tuple(category for category in options.categories if category in BROWSER_CATEGORIES)
        if options.audit_accounts or options.wipe_vscode_profile or options.browser_profile_paths or options.vscode_backup:
            return ()
        return BROWSER_CATEGORIES
