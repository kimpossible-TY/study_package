"""Data models for the browser cleaner application.

Defines structures for representing browser definitions, configuration options,
clean targets, and result summaries.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class BrowserDefinition:
    """Configuration definition for a specific web browser.

    Attributes:
        key: Unique lowercase identifier for the browser (e.g., 'chrome').
        display_name: Human-readable name (e.g., 'Google Chrome').
        process_names: Executable names to check for running instances (e.g., ('chrome.exe',)).
        profile_roots: Directories where profile folders are located.
        file_patterns: File names or glob patterns of files to clean.
        directory_patterns: Directory names or glob patterns of directories to clean.
        profile_strategy: Profile detection strategy ('chromium' or 'firefox').
    """

    key: str
    display_name: str
    process_names: tuple[str, ...]
    profile_roots: tuple[Path, ...]
    file_patterns: tuple[str, ...]
    directory_patterns: tuple[str, ...]
    profile_strategy: str


@dataclass(frozen=True)
class CleanerOptions:
    """Execution options specified via the command line or UI.

    Attributes:
        apply_changes: If True, actually delete target files; otherwise, dry-run only.
        skip_confirmation: If True, skip user confirmation prompts when applying changes.
        close_browsers: If True, automatically terminate running instances of target browsers.
        browser_key: Filter execution to a single browser key (e.g., 'chrome') or 'all'.
    """

    apply_changes: bool
    skip_confirmation: bool
    close_browsers: bool
    browser_key: str


@dataclass(frozen=True)
class CleanTarget:
    """A specific file system path flagged for cleaning.

    Attributes:
        browser_name: The display name of the associated browser.
        profile_name: The name of the specific user profile directory.
        path: Absolute path to the file or directory to be deleted.
    """

    browser_name: str
    profile_name: str
    path: Path


@dataclass(frozen=True)
class CleanResult:
    """The outcome of a cleaning attempt on a specific CleanTarget.

    Attributes:
        target: The target that was cleaned/scanned.
        status: The result status (e.g., 'removed', 'dry-run', 'locked', 'error', 'missing').
        details: Human-readable context or error messages.
    """

    target: CleanTarget
    status: str
    details: str

