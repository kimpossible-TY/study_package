"""Browser profile discovery and target path collection.

Locates user profiles for Firefox and Chromium-based browsers, and collects target
files and directories scheduled for deletion within those profiles.
"""

from __future__ import annotations

import stat
from pathlib import Path

from .config import TargetPatterns
from .models import BROWSER_CATEGORIES, BrowserDefinition, CleanTarget


class ProfileFinder:
    """Finds user profiles and filters potential clean targets based on configuration."""

    def find_profiles(self, browser: BrowserDefinition) -> list[Path]:
        """Scans profile roots of a browser to find individual user profile directories.

        Args:
            browser: The BrowserDefinition to search profiles for.

        Returns:
            A sorted list of profile directory paths.
        """
        profiles: list[Path] = []
        for root in browser.profile_roots:
            try:
                if not root.exists() or self._is_link_or_reparse(root):
                    continue
                profiles.extend(self._profiles_in_root(browser, root))
            except OSError:
                # One inaccessible browser root must not hide other browsers or
                # profiles from a dry-run/audit result.
                continue
        # Sort profiles alphabetically for consistent output order
        return sorted(profiles, key=lambda path: path.name.lower())

    def find_targets(
        self,
        browser: BrowserDefinition,
        profile: Path,
        categories: tuple[str, ...] = BROWSER_CATEGORIES,
    ) -> list[CleanTarget]:
        """Locates all matching clean targets (files & directories) in a given profile.

        Args:
            browser: The BrowserDefinition defining the patterns to check.
            profile: The path to the specific user profile directory.

        Returns:
            A sorted list of CleanTarget objects. Files are sorted before directories
            to clean individual files first.
        """
        targets: list[CleanTarget] = []
        for category, patterns in TargetPatterns.category_patterns(browser.profile_strategy, categories).items():
            for pattern in patterns["files"]:
                self._append_if_exists(targets, browser, profile, profile / pattern, category)
            for pattern in patterns["directories"]:
                self._append_if_exists(targets, browser, profile, profile / pattern, category)
        # Sort targets: files first, then directories, sorted alphabetically by path string
        return sorted(targets, key=lambda target: (target.path.is_dir(), str(target.path).lower()))

    def _profiles_in_root(self, browser: BrowserDefinition, root: Path) -> list[Path]:
        """Discovers profiles inside a specific root path according to profile strategy.

        Firefox matches any sub-directory in the root, while Chromium matches
        specific naming conventions ('Default', 'Profile *', etc.).
        """
        try:
            candidates = list(root.iterdir())
        except OSError:
            return []
        if browser.profile_strategy == "firefox":
            return [path for path in candidates if path.is_dir() and not self._is_link_or_reparse(path)]
        return [
            path for path in candidates
            if path.is_dir() and not self._is_link_or_reparse(path) and self._is_chromium_profile(path)
        ]

    def _is_chromium_profile(self, path: Path) -> bool:
        """Determines if a directory matches a Chromium user profile signature."""
        return (
            path.name == "Default"
            or path.name.startswith("Profile ")
            or path.name in {"Guest Profile", "System Profile"}
        )

    def _append_if_exists(
        self,
        targets: list[CleanTarget],
        browser: BrowserDefinition,
        profile: Path,
        path: Path,
        category: str,
    ) -> None:
        """Appends a CleanTarget to the list if the physical file or folder exists."""
        if path.exists() and not self._is_link_or_reparse(path):
            targets.append(
                CleanTarget(
                    browser_name=browser.display_name,
                    profile_name=profile.name,
                    path=path,
                    category=category,
                    allowed_root=profile,
                )
            )

    def _is_link_or_reparse(self, path: Path) -> bool:
        """Return whether a path is a symlink or Windows reparse point without following it."""
        try:
            path_stat = path.lstat()
        except OSError:
            return True
        attributes = getattr(path_stat, "st_file_attributes", 0)
        reparse = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
        return path.is_symlink() or bool(attributes & reparse)
