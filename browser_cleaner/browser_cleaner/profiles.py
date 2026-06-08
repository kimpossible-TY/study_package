"""Browser profile discovery and target path collection.

Locates user profiles for Firefox and Chromium-based browsers, and collects target
files and directories scheduled for deletion within those profiles.
"""

from __future__ import annotations

from pathlib import Path

from .models import BrowserDefinition, CleanTarget


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
            if not root.exists():
                continue
            profiles.extend(self._profiles_in_root(browser, root))
        # Sort profiles alphabetically for consistent output order
        return sorted(profiles, key=lambda path: path.name.lower())

    def find_targets(self, browser: BrowserDefinition, profile: Path) -> list[CleanTarget]:
        """Locates all matching clean targets (files & directories) in a given profile.

        Args:
            browser: The BrowserDefinition defining the patterns to check.
            profile: The path to the specific user profile directory.

        Returns:
            A sorted list of CleanTarget objects. Files are sorted before directories
            to clean individual files first.
        """
        targets: list[CleanTarget] = []
        for pattern in browser.file_patterns:
            self._append_if_exists(targets, browser, profile, profile / pattern)
        for pattern in browser.directory_patterns:
            self._append_if_exists(targets, browser, profile, profile / pattern)
        # Sort targets: files first, then directories, sorted alphabetically by path string
        return sorted(targets, key=lambda target: (target.path.is_dir(), str(target.path).lower()))

    def _profiles_in_root(self, browser: BrowserDefinition, root: Path) -> list[Path]:
        """Discovers profiles inside a specific root path according to profile strategy.

        Firefox matches any sub-directory in the root, while Chromium matches
        specific naming conventions ('Default', 'Profile *', etc.).
        """
        if browser.profile_strategy == "firefox":
            return [path for path in root.iterdir() if path.is_dir()]
        return [path for path in root.iterdir() if path.is_dir() and self._is_chromium_profile(path)]

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
    ) -> None:
        """Appends a CleanTarget to the list if the physical file or folder exists."""
        if path.exists():
            targets.append(
                CleanTarget(
                    browser_name=browser.display_name,
                    profile_name=profile.name,
                    path=path,
                )
            )

