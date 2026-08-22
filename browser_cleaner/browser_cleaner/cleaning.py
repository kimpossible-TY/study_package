"""File and directory cleaning utility.

Handles safe removal of files and directories corresponding to browser artifacts,
returning structured results representing success, dry-run, or errors.
"""

from __future__ import annotations

import os
import stat
from pathlib import Path

from .models import CleanResult, CleanTarget


class PathCleaner:
    """Utility to clean specific files or directories from the filesystem."""

    def clean(self, target: CleanTarget, apply_changes: bool) -> CleanResult:
        """Attempts to delete the path specified by the target.

        Args:
            target: The CleanTarget representing the file or directory to delete.
            apply_changes: If True, executes the deletion; otherwise, simulates it.

        Returns:
            A CleanResult describing the outcome of the clean operation.
        """
        try:
            target.path.lstat()
        except FileNotFoundError:
            return CleanResult(target=target, status="missing", details="already gone")
        except OSError as exc:
            return CleanResult(target=target, status="error", details=str(exc))

        unsafe_reason = self._unsafe_reason(target)
        if unsafe_reason:
            return CleanResult(target=target, status="error", details=unsafe_reason)

        # If in dry-run mode, report what would be deleted without actually doing it.
        if not apply_changes:
            return CleanResult(target=target, status="dry-run", details="would remove")

        try:
            if target.path.is_dir():
                # Preflight avoids following a symlink or Windows junction buried in
                # a cache/local-storage tree.  A refusal leaves the tree unchanged.
                self._assert_tree_has_no_reparse_points(target.path)
                self._remove_tree(target.path)
            else:
                target.path.unlink()
            return CleanResult(target=target, status="removed", details="deleted")
        except PermissionError:
            # Typically happens if the browser is running and files are locked by the process
            return CleanResult(target=target, status="locked", details="close the browser and retry")
        except FileNotFoundError:
            # The file/directory might have been deleted by the browser or another process
            return CleanResult(target=target, status="missing", details="already gone")
        except OSError as exc:
            # Catch-all for any other OS errors (disk issues, permissions, etc.)
            return CleanResult(target=target, status="error", details=str(exc))

    def _unsafe_reason(self, target: CleanTarget) -> str | None:
        """Reject anything outside the discovered profile or involving indirection."""
        if target.allowed_root is None:
            return "missing approved profile root; refusing deletion"
        root = self._absolute_lexical(target.allowed_root)
        path = self._absolute_lexical(target.path)
        try:
            path.relative_to(root)
        except ValueError:
            return "target is outside the approved profile root; refusing deletion"
        for component in self._path_components(root, path):
            if self._is_link_or_reparse(component):
                return "symlink or Windows reparse point detected; refusing deletion"
        return None

    def _assert_tree_has_no_reparse_points(self, root: Path) -> None:
        """Walk with ``lstat`` only; do not let recursive deletion traverse links."""
        for current, directories, files in os.walk(root, topdown=True, followlinks=False):
            current_path = Path(current)
            for name in directories + files:
                candidate = current_path / name
                if self._is_link_or_reparse(candidate):
                    raise OSError("symlink or Windows reparse point inside target; refusing deletion")

    def _remove_tree(self, root: Path) -> None:
        """Remove a preflighted tree without using a link-following operation."""
        for child in root.iterdir():
            child_stat = child.lstat()
            if self._is_link_or_reparse(child):
                raise OSError("symlink or Windows reparse point appeared during deletion; stopping")
            if stat.S_ISDIR(child_stat.st_mode):
                self._remove_tree(child)
            else:
                child.unlink()
        root.rmdir()

    def _path_components(self, root: Path, path: Path) -> list[Path]:
        relative = path.relative_to(root)
        components = [root]
        current = root
        for part in relative.parts:
            current = current / part
            components.append(current)
        return components

    def _absolute_lexical(self, path: Path) -> Path:
        # ``resolve`` would follow a symlink before we can reject it.  ``abspath``
        # normalizes dot segments only and preserves the path identity for checks.
        return Path(os.path.abspath(os.path.normpath(os.fspath(path))))

    def _is_link_or_reparse(self, path: Path) -> bool:
        path_stat = path.lstat()
        attributes = getattr(path_stat, "st_file_attributes", 0)
        reparse = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
        return path.is_symlink() or bool(attributes & reparse)
