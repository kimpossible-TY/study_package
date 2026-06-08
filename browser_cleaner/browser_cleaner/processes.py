"""Process management for running browsers.

Handles finding and terminating active browser processes using Windows-specific
utilities (`tasklist` and `taskkill`).
"""

from __future__ import annotations

import subprocess
from typing import Iterable


class BrowserProcessManager:
    """Manages system processes associated with browsers to ensure safe file operations."""

    def running_processes(self, process_names: Iterable[str]) -> set[str]:
        """Queries the OS to identify which of the specified browser processes are currently running.

        Args:
            process_names: Iterable of process executable names (e.g., ['chrome.exe']).

        Returns:
            A set of lowercase process names that were found running.
        """
        wanted = {name.lower() for name in process_names}
        try:
            # Query active processes on Windows in CSV format with no headers (/nh)
            completed = subprocess.run(
                ["tasklist", "/fo", "csv", "/nh"],
                capture_output=True,
                text=True,
                check=False,
                encoding="utf-8",
                errors="replace",
            )
        except FileNotFoundError:
            # On non-Windows platforms, tasklist will not be found
            return set()

        running: set[str] = set()
        for line in completed.stdout.splitlines():
            process_name = self._parse_process_name(line)
            if process_name in wanted:
                running.add(process_name)
        return running

    def close_processes(self, process_names: Iterable[str]) -> None:
        """Forcefully terminates all instances of the specified process names.

        Args:
            process_names: Iterable of process executable names to close.
        """
        for process_name in process_names:
            # Run taskkill with force flag (/f) and image name (/im)
            subprocess.run(
                ["taskkill", "/f", "/im", process_name],
                capture_output=True,
                text=True,
                check=False,
            )

    def _parse_process_name(self, tasklist_line: str) -> str:
        """Parses the process name from a single CSV line returned by tasklist.

        Args:
            tasklist_line: CSV line from tasklist output.

        Returns:
            The parsed process name in lowercase, or empty string if invalid.
        """
        if not tasklist_line.strip():
            return ""
        # The first column of tasklist CSV is the image/process name
        return tasklist_line.split(",", 1)[0].strip().strip('"').lower()

