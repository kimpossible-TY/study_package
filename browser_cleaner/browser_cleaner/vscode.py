"""Safe VS Code profile backup and explicit full-profile wiping.

VS Code does not provide a stable non-interactive CLI logout for its accounts,
Copilot, Settings Sync, extension authentication, or UI-activated tunnels.
Accordingly this module never edits VS Code SQLite/JSON authentication state.
"""

from __future__ import annotations

import os
import shutil
import stat
from pathlib import Path
from typing import Callable

from .cleaning import PathCleaner
from .models import ActionResult, CleanTarget
from .processes import BrowserProcessManager


class VscodeProfileManager:
    """Operate only on known current-user VS Code data roots."""

    _profiles = (("VS Code Stable", "Code"), ("VS Code Insiders", "Code - Insiders"))

    def __init__(
        self,
        app_data: Path | None = None,
        user_profile: Path | None = None,
        cleaner: PathCleaner | None = None,
        process_manager: BrowserProcessManager | None = None,
    ) -> None:
        app_data_value = os.environ.get("APPDATA")
        user_profile_value = os.environ.get("USERPROFILE")
        self._app_data = app_data if app_data is not None else (Path(app_data_value) if app_data_value else None)
        self._user_profile = user_profile if user_profile is not None else (Path(user_profile_value) if user_profile_value else None)
        self._cleaner = cleaner or PathCleaner()
        self._process_manager = process_manager or BrowserProcessManager()

    def roots(self) -> list[tuple[str, Path]]:
        if self._app_data is None:
            return []
        return [(name, self._app_data / directory) for name, directory in self._profiles]

    def backup(self, destination: Path) -> list[ActionResult]:
        """Export only safe, account-free user customizations to a new directory."""
        if destination.exists():
            return [self._result("VS Code backup", str(destination), str(destination), "error", "backup destination already exists; refusing to overwrite")]
        if self._is_link_or_reparse(destination.parent):
            return [self._result("VS Code backup", str(destination), str(destination.parent), "error", "backup parent is a symlink or reparse point")]
        try:
            destination.mkdir(parents=True, exist_ok=False)
            copied = 0
            for name, root in self.roots():
                copied += self._backup_profile_files(name, root, destination)
            copied += self._backup_extension_names(destination)
            return [self._result("VS Code backup", str(destination), str(destination), "backed-up", f"exported {copied} safe customization artifact(s); auth stores were excluded")]
        except OSError as exc:
            return [self._result("VS Code backup", str(destination), str(destination), "error", str(exc))]

    def wipe(self, apply_changes: bool, confirm: Callable[[str, str], bool]) -> list[ActionResult]:
        """Remove full known VS Code data roots only after an unskippable prompt."""
        results: list[ActionResult] = []
        running = self._process_manager.running_processes(("Code.exe", "Code - Insiders.exe", "code.exe", "code-insiders.exe"))
        for display_name, root in self.roots():
            if not root.exists():
                results.append(self._result(display_name, "local user-data profile", str(root), "missing", "profile directory not found"))
                continue
            if running:
                results.append(self._result(display_name, "local user-data profile", str(root), "locked", "VS Code appears to be running; close it normally before retrying"))
                continue
            if not apply_changes:
                results.append(self._result(display_name, "local user-data profile", str(root), "dry-run", "would remove settings, keybindings, snippets, workspace state, extension state, and local chat/session data"))
                continue
            phrase = f"WIPE VSCODE PROFILE {root}"
            if not confirm(f"{display_name} local profile at '{root}'", phrase):
                results.append(self._result(display_name, "local user-data profile", str(root), "manual", "strong confirmation declined"))
                continue
            clean_result = self._cleaner.clean(
                CleanTarget(display_name, "local user-data profile", root, "full-profile", root),
                apply_changes=True,
            )
            results.append(self._result(display_name, "local user-data profile", str(root), clean_result.status, clean_result.details))
        return results

    def _backup_profile_files(self, display_name: str, root: Path, destination: Path) -> int:
        user_directory = root / "User"
        if not user_directory.is_dir() or self._is_link_or_reparse(user_directory):
            return 0
        output = destination / self._safe_directory_name(display_name)
        output.mkdir(exist_ok=True)
        copied = 0
        for filename in ("settings.json", "keybindings.json"):
            source = user_directory / filename
            if source.is_file() and not self._is_link_or_reparse(source):
                shutil.copy2(source, output / filename)
                copied += 1
        snippets = user_directory / "snippets"
        if snippets.is_dir() and not self._is_link_or_reparse(snippets) and self._tree_is_safe(snippets):
            shutil.copytree(snippets, output / "snippets", symlinks=False)
            copied += 1
        return copied

    def _backup_extension_names(self, destination: Path) -> int:
        if self._user_profile is None:
            return 0
        artifacts = 0
        for label, directory in (("stable", self._user_profile / ".vscode" / "extensions"), ("insiders", self._user_profile / ".vscode-insiders" / "extensions")):
            if not directory.is_dir() or self._is_link_or_reparse(directory):
                continue
            names = sorted(
                child.name for child in directory.iterdir() if child.is_dir() and not self._is_link_or_reparse(child)
            )
            (destination / f"extensions-{label}.txt").write_text("\n".join(names) + ("\n" if names else ""), encoding="utf-8")
            artifacts += 1
        return artifacts

    def _tree_is_safe(self, root: Path) -> bool:
        for current, directories, files in os.walk(root, topdown=True, followlinks=False):
            for name in directories + files:
                if self._is_link_or_reparse(Path(current) / name):
                    return False
        return True

    def _is_link_or_reparse(self, path: Path) -> bool:
        try:
            path_stat = path.lstat()
        except OSError:
            return True
        attributes = getattr(path_stat, "st_file_attributes", 0)
        reparse = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
        return path.is_symlink() or bool(attributes & reparse)

    @staticmethod
    def _safe_directory_name(value: str) -> str:
        return value.replace(" ", "-").lower()

    @staticmethod
    def _result(provider: str, subject: str, location: str, status: str, details: str) -> ActionResult:
        return ActionResult(provider, subject, location, "full-profile", status, details)
