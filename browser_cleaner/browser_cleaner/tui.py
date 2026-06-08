"""Terminal User Interface (TUI) for interactive cleaning feedback.

Provides ANSI escape color formatting, structured header rendering, step/warning indicators,
and results/summary tables suited for a command-line interface.
"""

from __future__ import annotations

import os
import sys

from .models import CleanResult


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
            print(f"{status}  {target.browser_name} / {target.profile_name} / {target.path}")
            # Do not display redundant details for straightforward statuses
            if result.details not in {"deleted", "would remove"}:
                print(f"{' ' * status_width}  {self._color(result.details, Colors.DIM)}")
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

    def _color(self, text: str, color_code: str) -> str:
        """Wraps text with the given ANSI color code and reset sequence."""
        return f"{color_code}{text}{Colors.RESET}"

    def _status_color(self, status: str) -> str:
        """Selects a semantic color code matching the clean status."""
        if status == "removed":
            return Colors.GREEN
        if status == "dry-run":
            return Colors.YELLOW
        if status in {"locked", "error"}:
            return Colors.RED
        return Colors.DIM

