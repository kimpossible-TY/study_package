"""Remove only the PowerShell profile block owned by computer_setting.ps1."""

from __future__ import annotations

import codecs
import ctypes
import os
import re
from pathlib import Path
from typing import Callable

from .cleaning import PathCleaner
from .models import AccountAuditEntry, ActionResult, CleanTarget, VerificationEntry


class PowerShellProfileManager:
    begin = "# BEGIN study_package PowerShell settings"
    end = "# END study_package PowerShell settings"
    pattern = re.compile(
        r"^# BEGIN study_package PowerShell settings\r?\n.*?"
        r"^# END study_package PowerShell settings(?:\r?\n|$)",
        re.MULTILINE | re.DOTALL,
    )

    def __init__(self, documents: Path | None = None) -> None:
        self._documents = documents if documents is not None else self._find_documents()

    @staticmethod
    def _find_documents() -> Path | None:
        if os.name != "nt":
            return None
        # CSIDL_PERSONAL honors redirected/OneDrive Documents, like .NET
        # Environment.GetFolderPath('MyDocuments') in the bootstrapper.
        buffer = ctypes.create_unicode_buffer(32768)
        if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buffer) != 0:
            return None
        return Path(buffer.value) if buffer.value else None

    def paths(self) -> list[Path]:
        if self._documents is None:
            return []
        return [self._documents / edition / "profile.ps1" for edition in ("WindowsPowerShell", "PowerShell")]

    def _read(self, path: Path) -> tuple[str, str]:
        # Check ancestors too: never read/write through a symlink or junction.
        reason = PathCleaner()._unsafe_reason(CleanTarget("PowerShell", path.name, path, "developer-auth", Path(path.anchor)))
        if reason:
            raise OSError(reason)
        data = path.read_bytes()
        encoding = "utf-16" if data.startswith((codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE)) else "utf-8-sig" if data.startswith(codecs.BOM_UTF8) else "utf-8"
        return data.decode(encoding), encoding

    def audit(self) -> list[AccountAuditEntry]:
        entries = []
        for result in self.remove(False, lambda *_: False):
            entries.append(AccountAuditEntry("PowerShell profile", "study_package settings / mac", result.location,
                                             "present" if result.status == "dry-run" else "unknown", "yes", "no"))
        return entries

    def remove(self, apply: bool, confirm: Callable[[str, str], bool]) -> list[ActionResult]:
        results = []
        for path in self.paths():
            status, details = "error", ""
            try:
                content, encoding = self._read(path)
                if self.begin not in content and self.end not in content:
                    continue
                if content.count(self.begin) != 1 or content.count(self.end) != 1 or not self.pattern.search(content):
                    raise ValueError("invalid managed block; profile left unchanged")
                if not apply:
                    status, details = "dry-run", "would remove study_package settings and mac; preserve other profile content"
                elif not confirm(f"study_package settings in '{path}'", f"REMOVE POWERSHELL SETTINGS {path}"):
                    status, details = "manual", "confirmation declined"
                else:
                    remaining = self.pattern.sub("", content)
                    if remaining.strip():
                        path.write_bytes(remaining.encode(encoding))
                    else:
                        path.unlink()
                    status, details = "removed", "removed managed settings; close open PowerShell sessions to unload functions and prompt"
            except FileNotFoundError:
                continue
            except (OSError, UnicodeError, ValueError):
                # Never display profile contents or decoding exception payloads.
                details = "could not safely read or edit managed profile; profile left unchanged"
            results.append(ActionResult("PowerShell profile", "study_package settings / mac", str(path), "developer-auth", status, details))
        return results

    def verify(self) -> list[VerificationEntry]:
        if self._documents is None:
            return [VerificationEntry("PowerShell profile", "current-user Documents", "manual verification required", "Windows Documents location unavailable")]
        pending = {result.location: result for result in self.remove(False, lambda *_: False)}
        return [VerificationEntry("PowerShell profile", str(path),
                                  "manual verification required" if str(path) in pending else "no local evidence found",
                                  "managed settings remain or profile could not be checked" if str(path) in pending else "no managed settings found; close existing shells to unload session state")
                for path in self.paths()]
