from __future__ import annotations

import shutil

from .models import CleanResult, CleanTarget


class PathCleaner:
    def clean(self, target: CleanTarget, apply_changes: bool) -> CleanResult:
        if not apply_changes:
            return CleanResult(target=target, status="dry-run", details="would remove")

        try:
            if target.path.is_dir():
                shutil.rmtree(target.path)
            else:
                target.path.unlink()
            return CleanResult(target=target, status="removed", details="deleted")
        except PermissionError:
            return CleanResult(target=target, status="locked", details="close the browser and retry")
        except FileNotFoundError:
            return CleanResult(target=target, status="missing", details="already gone")
        except OSError as exc:
            return CleanResult(target=target, status="error", details=str(exc))
