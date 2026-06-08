from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class BrowserDefinition:
    key: str
    display_name: str
    process_names: tuple[str, ...]
    profile_roots: tuple[Path, ...]
    file_patterns: tuple[str, ...]
    directory_patterns: tuple[str, ...]
    profile_strategy: str


@dataclass(frozen=True)
class CleanerOptions:
    apply_changes: bool
    skip_confirmation: bool
    close_browsers: bool
    browser_key: str


@dataclass(frozen=True)
class CleanTarget:
    browser_name: str
    profile_name: str
    path: Path


@dataclass(frozen=True)
class CleanResult:
    target: CleanTarget
    status: str
    details: str
