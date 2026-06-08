from __future__ import annotations

import os
import sys

from .models import CleanResult


class Colors:
    RESET = "\033[0m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    CYAN = "\033[36m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    RED = "\033[31m"
    BLUE = "\033[34m"


class TerminalUI:
    def __init__(self, width: int = 72) -> None:
        self._width = width

    def setup(self) -> None:
        if sys.stdout.isatty() and os.name == "nt":
            os.system("")

    def header(self, title: str, subtitle: str) -> None:
        line = "=" * self._width
        print(self._color(line, Colors.BLUE))
        print(self._color(f"{title:^{self._width}}", Colors.BOLD + Colors.CYAN))
        print(self._color(f"{subtitle:^{self._width}}", Colors.DIM))
        print(self._color(line, Colors.BLUE))

    def step(self, message: str) -> None:
        print(f"{self._color('>', Colors.CYAN)} {message}")

    def warning(self, message: str) -> None:
        print(f"{self._color('!', Colors.YELLOW)} {message}")

    def success(self, message: str) -> None:
        print(f"{self._color('+', Colors.GREEN)} {message}")

    def error(self, message: str) -> None:
        print(f"{self._color('x', Colors.RED)} {message}")

    def confirm_apply(self, skip_prompt: bool) -> bool:
        if skip_prompt:
            return True
        print()
        self.warning("This will delete selected browser data from disk.")
        answer = input("Type CLEAN to continue: ").strip()
        return answer == "CLEAN"

    def print_results(self, results: list[CleanResult]) -> None:
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
            if result.details not in {"deleted", "would remove"}:
                print(f"{' ' * status_width}  {self._color(result.details, Colors.DIM)}")
        print(self._color("-" * self._width, Colors.BLUE))

    def print_summary(self, results: list[CleanResult], apply_changes: bool) -> None:
        counts: dict[str, int] = {}
        for result in results:
            counts[result.status] = counts.get(result.status, 0) + 1

        mode = "APPLY" if apply_changes else "DRY RUN"
        mode_color = Colors.GREEN if apply_changes else Colors.YELLOW
        print()
        print(self._color(f"Mode: {mode}", Colors.BOLD + mode_color))
        for key in ("removed", "dry-run", "locked", "error", "missing"):
            if key in counts:
                print(f"{key:>8}: {counts[key]}")

    def _color(self, text: str, color_code: str) -> str:
        return f"{color_code}{text}{Colors.RESET}"

    def _status_color(self, status: str) -> str:
        if status == "removed":
            return Colors.GREEN
        if status == "dry-run":
            return Colors.YELLOW
        if status in {"locked", "error"}:
            return Colors.RED
        return Colors.DIM
