from __future__ import annotations

import subprocess
from typing import Iterable


class BrowserProcessManager:
    def running_processes(self, process_names: Iterable[str]) -> set[str]:
        wanted = {name.lower() for name in process_names}
        try:
            completed = subprocess.run(
                ["tasklist", "/fo", "csv", "/nh"],
                capture_output=True,
                text=True,
                check=False,
                encoding="utf-8",
                errors="replace",
            )
        except FileNotFoundError:
            return set()

        running: set[str] = set()
        for line in completed.stdout.splitlines():
            process_name = self._parse_process_name(line)
            if process_name in wanted:
                running.add(process_name)
        return running

    def close_processes(self, process_names: Iterable[str]) -> None:
        for process_name in process_names:
            subprocess.run(
                ["taskkill", "/f", "/im", process_name],
                capture_output=True,
                text=True,
                check=False,
            )

    def _parse_process_name(self, tasklist_line: str) -> str:
        if not tasklist_line.strip():
            return ""
        return tasklist_line.split(",", 1)[0].strip().strip('"').lower()
