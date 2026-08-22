"""Data models for the browser cleaner application.

Defines structures for representing browser definitions, configuration options,
clean targets, and result summaries.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


BROWSER_CATEGORIES = (
    "sessions",
    "history",
    "passwords",
    "autofill",
    "site-data",
    "cache",
)

ALL_CATEGORIES = BROWSER_CATEGORIES + ("developer-auth", "full-profile")


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
        categories: Explicit cleanup categories. An empty tuple selects the
            two-command unified browser/developer-auth review/apply mode when
            the default browser selection is in use.
        audit_accounts: Only report locally discoverable account evidence.
        verify: Run post-cleanup checks without changing local data.
        wipe_vscode_profile: Remove the complete local VS Code user-data profile.
        vscode_backup: A directory into which the safe VS Code backup is written.
        browser_profile_paths: Exact browser-profile paths selected for a full wipe.
    """

    apply_changes: bool
    skip_confirmation: bool
    close_browsers: bool
    browser_key: str
    categories: tuple[str, ...] = ()
    audit_accounts: bool = False
    verify: bool = False
    wipe_vscode_profile: bool = False
    vscode_backup: Path | None = None
    browser_profile_paths: tuple[Path, ...] = ()


@dataclass(frozen=True)
class CleanTarget:
    """A specific file system path flagged for cleaning.

    Attributes:
        browser_name: The display name of the associated browser.
        profile_name: The name of the specific user profile directory.
        path: Absolute path to the file or directory to be deleted.
        category: The narrowly scoped cleanup category represented by ``path``.
        allowed_root: The profile directory which must lexically contain ``path``.
    """

    browser_name: str
    profile_name: str
    path: Path
    category: str = "site-data"
    allowed_root: Path | None = None


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


@dataclass(frozen=True)
class AccountAuditEntry:
    """A secret-free, locally observable account or authentication record."""

    provider: str
    account_name: str
    location: str
    login_status: str
    automatic_removal: str
    manual_action: str


@dataclass(frozen=True)
class ActionResult:
    """Result of a non-file developer-authentication operation."""

    provider: str
    subject: str
    location: str
    category: str
    status: str
    details: str


@dataclass(frozen=True)
class VerificationEntry:
    """A conservative conclusion from a local, non-destructive verification check."""

    subject: str
    location: str
    status: str
    details: str
