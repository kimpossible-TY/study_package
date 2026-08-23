"""Terminal User Interface (TUI) for interactive cleaning feedback.

The default presentation is intentionally compact: it highlights decisions and
actionable results, while ``--verbose`` keeps the old per-item tables available
for troubleshooting and audits.
"""

from __future__ import annotations

import os
import shutil
import sys
from collections import Counter

from .models import AccountAuditEntry, ActionResult, CleanResult, CleanTarget, VerificationEntry


class Colors:
    """ANSI Escape codes for colored terminal outputs."""

    RESET = "\033[0m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    CYAN = "\033[36m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    RED = "\033[31m"
    BLUE = "\033[34m"


class TerminalUI:
    """Handles rendering of warnings, errors, step updates, results tables, and confirmation prompts."""

    def __init__(self, width: int = 72, verbose: bool = False) -> None:
        """Initializes the TUI formatting parameters.

        Args:
            width: Width of headers and divider lines.
            verbose: Whether to show detailed progress and tables.
        """
        terminal_width = shutil.get_terminal_size((width, 24)).columns
        self._width = max(40, min(width, terminal_width))
        self._verbose = verbose
        self._color_enabled = bool(getattr(sys.stdout, "isatty", lambda: False)())
        self._missing_profile_browsers: list[str] = []

    @property
    def verbose(self) -> bool:
        """Whether detailed progress and table output is enabled."""
        return self._verbose

    def setup(self) -> None:
        """Initializes terminal properties.

        Under Windows, calling `os.system("")` enables ANSI escape sequence parsing in standard cmd.exe.
        """
        if sys.stdout.isatty() and os.name == "nt":
            os.system("")
        self._color_enabled = sys.stdout.isatty()

    def header(self, title: str, subtitle: str) -> None:
        """Prints a styled decorative header centered to the specified width.

        Args:
            title: Title text of the app.
            subtitle: Subtitle/description text.
        """
        print(self._color(title, Colors.BOLD + Colors.CYAN))
        if subtitle:
            print(self._color(subtitle, Colors.DIM))
        print()

    def step(self, message: str) -> None:
        """Prints an informational progress message."""
        if self._verbose:
            print(f"{self._color('•', Colors.CYAN)} {message}")

    def warning(self, message: str) -> None:
        """Prints a warning message to draw user attention."""
        if not self._verbose and message.startswith("No safe profiles found for "):
            browser_name = message.removeprefix("No safe profiles found for ").rstrip(".")
            self._missing_profile_browsers.append(browser_name)
            return
        print(f"{self._color('!', Colors.YELLOW)} {message}")

    def success(self, message: str) -> None:
        """Prints a success or completion message."""
        print(f"{self._color('✓', Colors.GREEN)} {message}")

    def error(self, message: str) -> None:
        """Prints an error or failure message."""
        print(f"{self._color('×', Colors.RED)} {message}")

    def confirm_apply(self, skip_prompt: bool) -> bool:
        """Prompts the user to type 'CLEAN' before performing deletions.

        Args:
            skip_prompt: If True, bypasses prompt and automatically returns True.

        Returns:
            True if the user confirmed deletion (or skipped); False otherwise.
        """
        if skip_prompt:
            return True
        print()
        self.warning("This will delete selected browser data from disk.")
        answer = input("Type CLEAN to continue: ").strip()
        return answer == "CLEAN"

    def confirm_sensitive(self, subject: str, phrase: str) -> bool:
        """Require a purpose-specific prompt that ``--yes`` can never bypass."""
        print()
        self.warning(f"Sensitive local-authentication or full-profile operation: {subject}")
        self.warning("This operation can be irreversible and may remove more than a website session.")
        answer = input(f"Type exactly '{phrase}' to continue: ").strip()
        return answer == phrase

    def print_results(self, results: list[CleanResult]) -> None:
        """Renders a formatted table of all clean actions taken or simulated.

        Args:
            results: List of CleanResult objects to print.
        """
        if not results:
            print()
            print(self._color("Browser cleanup", Colors.BOLD))
            self._flush_profile_warnings()
            print("  No matching browser data found.")
            return

        print()
        print(self._color("Browser cleanup", Colors.BOLD))
        self._flush_profile_warnings()

        if not self._verbose:
            counts = Counter(result.status for result in results)
            summary = " · ".join(f"{count} {status}" for status, count in counts.items())
            label = "target" if len(results) == 1 else "targets"
            print(f"  {len(results)} {label} · {summary}")
            for result in results:
                if result.status in {"locked", "error"}:
                    self._print_result_issue(result)
            return

        status_width = max(len(result.status) for result in results)
        print(self._color("-" * self._width, Colors.BLUE))
        for result in results:
            status = self._color(result.status.ljust(status_width), self._status_color(result.status))
            target = result.target
            print(f"{status}  [{target.category}] {target.browser_name} / {target.profile_name} / {target.path}")
            if result.details not in {"deleted", "would remove"}:
                print(f"{' ' * status_width}  {self._color(result.details, Colors.DIM)}")
        print(self._color("-" * self._width, Colors.BLUE))

    def print_targets(self, targets: list[CleanTarget]) -> None:
        """Show the exact normal-browser paths before an apply confirmation."""
        print()
        print(self._color("Selected browser targets — review before deletion", Colors.BOLD))
        if not targets:
            self.warning("No matching selected browser paths were found.")
        for target in targets:
            print(f"[{target.category}] {target.browser_name} / {target.profile_name} / {target.path}")
        print()

    def print_summary(self, results: list[CleanResult], apply_changes: bool) -> None:
        """Prints a counts summary of statuses (e.g. removed, dry-run, locked).

        Args:
            results: List of CleanResult objects.
            apply_changes: Specifies whether the run was live or dry-run.
        """
        counts: dict[str, int] = {}
        for result in results:
            counts[result.status] = counts.get(result.status, 0) + 1

        mode = "APPLY" if apply_changes else "DRY RUN"
        mode_color = Colors.GREEN if apply_changes else Colors.YELLOW
        print(self._color(f"{mode} · {'local changes applied' if apply_changes else 'nothing changed'}", Colors.BOLD + mode_color))
        if self._verbose:
            for key in ("removed", "dry-run", "locked", "error", "missing"):
                if key in counts:
                    print(f"{key:>8}: {counts[key]}")

    def print_audit(self, entries: list[AccountAuditEntry]) -> None:
        """Render only approved secret-free account-audit columns."""
        if not self._verbose:
            active = [
                entry
                for entry in entries
                if entry.automatic_removal == "yes"
                and entry.login_status not in {"unsupported", "not configured", "no local evidence found", "unknown"}
            ]
            manual_count = sum(entry.manual_action == "yes" for entry in entries)
            print()
            print(self._color("Local account check", Colors.BOLD))
            if active:
                for entry in active:
                    print(f"  {entry.provider} · {entry.account_name} · local sign-out available")
            else:
                print("  No active local sign-out candidates found.")
            if manual_count:
                print(f"  {manual_count} manual checks · use --audit-accounts for details")
            return

        print()
        print(self._color("Account audit (no secrets read or displayed)", Colors.BOLD))
        headers = ("Provider", "Account / username", "Location / target", "Login", "Auto remove", "Manual")
        rows = [
            (entry.provider, entry.account_name, entry.location, entry.login_status, entry.automatic_removal, entry.manual_action)
            for entry in entries
        ]
        self._print_table(headers, rows)

    def print_action_results(self, results: list[ActionResult]) -> None:
        if not results:
            return
        if not self._verbose:
            print()
            print(self._color("Developer authentication", Colors.BOLD))
            visible = [result for result in results if result.status not in {"unsupported", "missing"}]
            if visible:
                for result in visible:
                    print(f"  {result.status} · {result.provider} · {result.subject}")
            else:
                print("  No supported local sign-out actions available on this host.")
            unsupported = sum(result.status in {"unsupported", "missing"} for result in results)
            if unsupported:
                print(f"  {unsupported} checks unavailable · use --verbose for details")
            return

        print()
        print(self._color("Developer / profile results", Colors.BOLD))
        headers = ("Status", "Provider", "Account / target", "Location", "Details")
        rows = [(result.status, result.provider, result.subject, result.location, result.details) for result in results]
        self._print_table(headers, rows, status_column=True)

    def print_verification(self, entries: list[VerificationEntry]) -> None:
        if not entries:
            return
        if not self._verbose:
            counts = Counter(entry.status for entry in entries)
            print()
            print(self._color("Verification", Colors.BOLD))
            print("  " + " · ".join(f"{count} {status}" for status, count in counts.items()))
            for entry in entries:
                if entry.status in {"error", "manual verification required"}:
                    print(f"  {entry.status} · {entry.subject}")
            return

        print()
        print(self._color("Verification", Colors.BOLD))
        headers = ("Status", "Subject", "Location", "Details")
        rows = [(entry.status, entry.subject, entry.location, entry.details) for entry in entries]
        self._print_table(headers, rows, status_column=True)

    def print_vscode_manual_guidance(self) -> None:
        if not self._verbose:
            print("VS Code: account and sync sign-out is manual (Accounts → Settings Sync).")
            return
        print()
        self._color_print("VS Code manual account sign-out", Colors.BOLD)
        print("  1. In Stable and Insiders, open Accounts (bottom-left/profile menu) and sign out of GitHub and Microsoft accounts.")
        print("  2. Open Manage > Settings Sync, choose Turn Off, and turn sync off for this device before deleting local data.")
        print("  3. Check GitHub Copilot and extension account entries in Accounts; sign out there if shown.")
        print("  4. Run Command Palette > 'Remote Tunnels: Turn off Remote Tunnel Access...' for UI-managed tunnels.")

    def print_browser_sync_guidance(self) -> None:
        if not self._verbose:
            print("Browser sync: turn sync off and sign out in each profile before deleting site data.")
            return
        print()
        self.warning("Deleting cookies or local storage usually signs websites out, but does not reliably sign out browser sync accounts.")
        print("Turn sync off first, then sign out in each browser's profile/account menu: Chrome, Edge, Firefox, and Brave.")
        print("With sync active, deleting browser data can affect synced data on other devices.")

    def print_remote_revocation_guidance(self) -> None:
        if not self._verbose:
            print("Remote follow-up: review active sessions, OAuth apps, and tokens in the provider accounts.")
            return
        print()
        self._color_print("Remote follow-up (this tool never revokes remote sessions or tokens)", Colors.BOLD)
        print("Review GitHub OAuth Applications, personal access tokens, SSH keys, and active sessions.")
        print("Review Microsoft account devices/sessions and Google account devices/sessions.")

    def print_windows_profile_guidance(self) -> None:
        if not self._verbose:
            print("Device handoff: remove the old OS user profile separately after backing up needed files.")
            return
        print()
        print("For a device handoff, the most certain option is: back up files; create a separate local administrator; sign out;")
        print("sign in to that administrator; then use Settings > Accounts > Other users to remove the former user and its data.")

    def _print_table(self, headers: tuple[str, ...], rows: list[tuple[str, ...]], status_column: bool = False) -> None:
        """Keep output readable without emitting any command output or secret values."""
        print(self._color("-" * self._width, Colors.BLUE))
        print("  ".join(headers))
        print(self._color("-" * self._width, Colors.BLUE))
        for row in rows:
            rendered = list(row)
            if status_column:
                rendered[0] = self._color(rendered[0], self._status_color(rendered[0]))
            print("  ".join(rendered))
        print(self._color("-" * self._width, Colors.BLUE))

    def _color_print(self, text: str, color_code: str) -> None:
        print(self._color(text, color_code))

    def _color(self, text: str, color_code: str) -> str:
        """Wraps text with the given ANSI color code and reset sequence."""
        if not self._color_enabled:
            return text
        return f"{color_code}{text}{Colors.RESET}"

    def _flush_profile_warnings(self) -> None:
        if not self._missing_profile_browsers:
            return
        browsers = ", ".join(self._missing_profile_browsers)
        if len(self._missing_profile_browsers) == 1:
            print(f"  No browser profile found: {browsers}.")
        else:
            print(f"  No browser profiles found for {len(self._missing_profile_browsers)} browsers.")
        self._missing_profile_browsers.clear()

    def _print_result_issue(self, result: CleanResult) -> None:
        target = result.target
        print(f"  {result.status} · {target.browser_name} / {target.profile_name} · {result.details}")

    def _status_color(self, status: str) -> str:
        """Selects a semantic color code matching the clean status."""
        if status in {"removed", "verified removed"}:
            return Colors.GREEN
        if status in {"dry-run", "manual verification required"}:
            return Colors.YELLOW
        if status in {"locked", "error"}:
            return Colors.RED
        return Colors.DIM
