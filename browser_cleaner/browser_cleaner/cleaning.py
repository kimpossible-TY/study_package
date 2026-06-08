"""File and directory cleaning utility.

Handles safe removal of files and directories corresponding to browser artifacts,
returning structured results representing success, dry-run, or errors.
"""

from __future__ import annotations

import shutil

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
        # If in dry-run mode, report what would be deleted without actually doing it
        if not apply_changes:
            return CleanResult(target=target, status="dry-run", details="would remove")

        try:
            # Handle directory removal recursively
            if target.path.is_dir():
                shutil.rmtree(target.path)
            # Handle individual file removal
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

