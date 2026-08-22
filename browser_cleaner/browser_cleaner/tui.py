"""Terminal User Interface (TUI) for interactive cleaning feedback.

Provides ANSI escape color formatting, structured header rendering, step/warning indicators,
and results/summary tables suited for a command-line interface.
"""

from __future__ import annotations

import os
import sys

from .models import AccountAuditEntry, ActionResult, CleanResult, VerificationEntry


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

    def __init__(self, width: int = 72) -> None:
        """Initializes the TUI formatting parameters.

        Args:
            width: Width of headers and divider lines.
        """
        self._width = width

    def setup(self) -> None:
        """Initializes terminal properties.

        Under Windows, calling `os.system("")` enables ANSI escape sequence parsing in standard cmd.exe.
        """
        if sys.stdout.isatty() and os.name == "nt":
            os.system("")

    def header(self, title: str, subtitle: str) -> None:
        """Prints a styled decorative header centered to the specified width.

        Args:
            title: Title text of the app.
            subtitle: Subtitle/description text.
        """
        line = "=" * self._width
        print(self._color(line, Colors.BLUE))
        print(self._color(f"{title:^{self._width}}", Colors.BOLD + Colors.CYAN))
        print(self._color(f"{subtitle:^{self._width}}", Colors.DIM))
        print(self._color(line, Colors.BLUE))

    def step(self, message: str) -> None:
        """Prints an informational progress message."""
        print(f"{self._color('>', Colors.CYAN)} {message}")

    def warning(self, message: str) -> None:
        """Prints a warning message to draw user attention."""
        print(f"{self._color('!', Colors.YELLOW)} {message}")

    def success(self, message: str) -> None:
        """Prints a success or completion message."""
        print(f"{self._color('+', Colors.GREEN)} {message}")

    def error(self, message: str) -> None:
        """Prints an error or failure message."""
        print(f"{self._color('x', Colors.RED)} {message}")

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
            self.warning("No matching browser history, search, login, cookie, cache, or session data found.")
            return

        status_width = max(len(result.status) for result in results)
        print()
        print(self._color("Clean Results", Colors.BOLD))
        print(self._color("-" * self._width, Colors.BLUE))
        for result in results:
            status = self._color(result.status.ljust(status_width), self._status_color(result.status))
            target = result.target
            print(f"{status}  [{target.category}] {target.browser_name} / {target.profile_name} / {target.path}")
            # Do not display redundant details for straightforward statuses.
            if result.details not in {"deleted", "would remove"}:
                print(f"{' ' * status_width}  {self._color(result.details, Colors.DIM)}")
        print(self._color("-" * self._width, Colors.BLUE))

    def print_targets(self, targets: list[CleanTarget]) -> None:
        """Show the exact normal-browser paths before an apply confirmation."""
        print()
        print(self._color("Selected Browser Targets (review before deletion)", Colors.BOLD))
        print(self._color("-" * self._width, Colors.BLUE))
        if not targets:
            self.warning("No matching selected browser paths were found.")
        for target in targets:
            print(f"[{target.category}] {target.browser_name} / {target.profile_name} / {target.path}")
        print(self._color("-" * self._width, Colors.BLUE))

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
        print()
        print(self._color(f"Mode: {mode}", Colors.BOLD + mode_color))
        # Print results categorized by status in a specific layout
        for key in ("removed", "dry-run", "locked", "error", "missing"):
            if key in counts:
                print(f"{key:>8}: {counts[key]}")

    def print_audit(self, entries: list[AccountAuditEntry]) -> None:
        """Render only approved secret-free account-audit columns."""
        print()
        print(self._color("Account Audit (no secrets read or displayed)", Colors.BOLD))
        headers = ("Provider", "Account / username", "Location / target", "Login", "Auto remove", "Manual")
        rows = [
            (entry.provider, entry.account_name, entry.location, entry.login_status, entry.automatic_removal, entry.manual_action)
            for entry in entries
        ]
        self._print_table(headers, rows)

    def print_action_results(self, results: list[ActionResult]) -> None:
        if not results:
            return
        print()
        print(self._color("Developer / Profile Results", Colors.BOLD))
        headers = ("Status", "Provider", "Account / target", "Location", "Details")
        rows = [(result.status, result.provider, result.subject, result.location, result.details) for result in results]
        self._print_table(headers, rows, status_column=True)

    def print_verification(self, entries: list[VerificationEntry]) -> None:
        if not entries:
            return
        print()
        print(self._color("Verification", Colors.BOLD))
        headers = ("Status", "Subject", "Location", "Details")
        rows = [(entry.status, entry.subject, entry.location, entry.details) for entry in entries]
        self._print_table(headers, rows, status_column=True)

    def print_vscode_manual_guidance(self) -> None:
        print()
        self._color_print("VS Code manual account sign-out", Colors.BOLD)
        print("  1. In Stable and Insiders, open Accounts (bottom-left/profile menu) and sign out of GitHub and Microsoft accounts.")
        print("  2. Open Manage > Settings Sync, choose Turn Off, and turn sync off for this device before deleting local data.")
        print("  3. Check GitHub Copilot and extension account entries in Accounts; sign out there if shown.")
        print("  4. Run Command Palette > 'Remote Tunnels: Turn off Remote Tunnel Access...' for UI-managed tunnels.")

    def print_browser_sync_guidance(self) -> None:
        print()
        self.warning("Deleting cookies or local storage usually signs websites out, but does not reliably sign out browser sync accounts.")
        print("Turn sync off first, then sign out in each browser's profile/account menu: Chrome, Edge, Firefox, and Brave.")
        print("With sync active, deleting browser data can affect synced data on other devices.")

    def print_remote_revocation_guidance(self) -> None:
        print()
        self._color_print("Remote follow-up (this tool never revokes remote sessions or tokens)", Colors.BOLD)
        print("Review GitHub OAuth Applications, personal access tokens, SSH keys, and active sessions.")
        print("Review Microsoft account devices/sessions and Google account devices/sessions.")

    def print_windows_profile_guidance(self) -> None:
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
        return f"{color_code}{text}{Colors.RESET}"

    def _status_color(self, status: str) -> str:
        """Selects a semantic color code matching the clean status."""
        if status in {"removed", "verified removed"}:
            return Colors.GREEN
        if status in {"dry-run", "manual verification required"}:
            return Colors.YELLOW
        if status in {"locked", "error"}:
            return Colors.RED
        return Colors.DIM
