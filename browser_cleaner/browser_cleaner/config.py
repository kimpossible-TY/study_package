"""Configuration catalog and target search patterns.

Defines the directory roots, process names, and file/directory name patterns
used to scan and clean each supported browser (Chrome, Edge, Firefox, Brave).
"""

from __future__ import annotations

import os
from pathlib import Path

from .models import BROWSER_CATEGORIES, BrowserDefinition


class BrowserCatalog:
    """Manages the catalog of supported browsers and their cleaning configurations."""

    def __init__(self) -> None:
        """Initializes the browser catalog by building definitions from environment paths."""
        self._browsers = self._build_catalog()

    def all(self) -> list[BrowserDefinition]:
        """Returns all configured browser definitions.

        Returns:
            A list of BrowserDefinition instances.
        """
        return list(self._browsers)

    def select(self, browser_key: str) -> list[BrowserDefinition]:
        """Filters the browser catalog by a specific browser key.

        Args:
            browser_key: The target browser key (e.g., 'chrome') or 'all'.

        Returns:
            A list containing the matched BrowserDefinition(s).
        """
        if browser_key == "all":
            return self.all()
        return [browser for browser in self._browsers if browser.key == browser_key]

    def keys(self) -> tuple[str, ...]:
        """Gets all browser keys in the catalog.

        Returns:
            A tuple of string keys (e.g., ('chrome', 'edge', ...)).
        """
        return tuple(browser.key for browser in self._browsers)


    def _build_catalog(self) -> list[BrowserDefinition]:
        """Constructs the browser catalog definitions, resolving OS environment variables.

        Returns:
            A list of configured BrowserDefinition objects.
        """
        local_app_data = self._environment_path("LOCALAPPDATA")
        app_data = self._environment_path("APPDATA")
        chromium_files = TargetPatterns.chromium_files()
        chromium_directories = TargetPatterns.chromium_directories()

        return [
            BrowserDefinition(
                key="chrome",
                display_name="Google Chrome",
                process_names=("chrome.exe",),
                profile_roots=((local_app_data / "Google" / "Chrome" / "User Data",) if local_app_data else ()),
                file_patterns=chromium_files,
                directory_patterns=chromium_directories,
                profile_strategy="chromium",
            ),
            BrowserDefinition(
                key="edge",
                display_name="Microsoft Edge",
                process_names=("msedge.exe",),
                profile_roots=((local_app_data / "Microsoft" / "Edge" / "User Data",) if local_app_data else ()),
                file_patterns=chromium_files,
                directory_patterns=chromium_directories,
                profile_strategy="chromium",
            ),
            BrowserDefinition(
                key="firefox",
                display_name="Mozilla Firefox",
                process_names=("firefox.exe",),
                profile_roots=((app_data / "Mozilla" / "Firefox" / "Profiles",) if app_data else ()),
                file_patterns=TargetPatterns.firefox_files(),
                directory_patterns=TargetPatterns.firefox_directories(),
                profile_strategy="firefox",
            ),
            BrowserDefinition(
                key="brave",
                display_name="Brave",
                process_names=("brave.exe",),
                profile_roots=((local_app_data / "BraveSoftware" / "Brave-Browser" / "User Data",) if local_app_data else ()),
                file_patterns=chromium_files,
                directory_patterns=chromium_directories,
                profile_strategy="chromium",
            ),
        ]

    @staticmethod
    def _environment_path(name: str) -> Path | None:
        value = os.environ.get(name)
        return Path(value) if value else None


class TargetPatterns:
    """Provides category-to-pattern maps for different browser families.

    The old cleaner used one undifferentiated tuple.  Keeping patterns in a
    category map makes a cookie/site-data request unable to also select history,
    passwords, autofill, cache, or sessions.
    """

    @staticmethod
    def chromium_files() -> tuple[str, ...]:
        """Compatibility flattening of all Chromium file patterns."""
        return TargetPatterns._flatten(TargetPatterns.chromium_category_patterns(), "files")

    @staticmethod
    def chromium_directories() -> tuple[str, ...]:
        """Compatibility flattening of all Chromium directory patterns."""
        return TargetPatterns._flatten(TargetPatterns.chromium_category_patterns(), "directories")

    @staticmethod
    def firefox_files() -> tuple[str, ...]:
        """Compatibility flattening of all Firefox file patterns."""
        return TargetPatterns._flatten(TargetPatterns.firefox_category_patterns(), "files")

    @staticmethod
    def firefox_directories() -> tuple[str, ...]:
        """Compatibility flattening of all Firefox directory patterns."""
        return TargetPatterns._flatten(TargetPatterns.firefox_category_patterns(), "directories")

    @staticmethod
    def chromium_category_patterns() -> dict[str, dict[str, tuple[str, ...]]]:
        return {
            "history": {
                "files": (
                    "History", "History-journal", "Network Action Predictor",
                    "Network Action Predictor-journal", "Shortcuts", "Shortcuts-journal",
                    "Top Sites", "Top Sites-journal", "Visited Links", "Favicons", "Favicons-journal",
                ),
                "directories": (),
            },
            "passwords": {"files": ("Login Data", "Login Data-journal"), "directories": ()},
            "autofill": {"files": ("Web Data", "Web Data-journal"), "directories": ()},
            "site-data": {
                "files": ("Cookies", "Cookies-journal", "Network/Cookies", "Network/Cookies-journal"),
                "directories": ("Local Storage", "IndexedDB", "Service Worker", "File System", "blob_storage"),
            },
            "cache": {
                "files": (),
                "directories": ("Cache", "Code Cache", "GPUCache", "Media Cache", "DawnCache", "GrShaderCache", "ShaderCache"),
            },
            "sessions": {"files": (), "directories": ("Session Storage", "Sessions")},
        }

    @staticmethod
    def firefox_category_patterns() -> dict[str, dict[str, tuple[str, ...]]]:
        return {
            "history": {
                "files": (
                    "places.sqlite", "places.sqlite-wal", "places.sqlite-shm", "favicons.sqlite",
                    "favicons.sqlite-wal", "favicons.sqlite-shm",
                ),
                "directories": (),
            },
            "passwords": {"files": ("signons.sqlite", "key4.db", "logins.json"), "directories": ()},
            "autofill": {"files": ("formhistory.sqlite", "formhistory.sqlite-wal", "formhistory.sqlite-shm"), "directories": ()},
            "site-data": {
                "files": ("cookies.sqlite", "cookies.sqlite-wal", "cookies.sqlite-shm", "content-prefs.sqlite", "content-prefs.sqlite-wal", "content-prefs.sqlite-shm"),
                "directories": ("storage",),
            },
            "cache": {"files": (), "directories": ("cache2", "startupCache", "datareporting", "jumpListCache")},
            "sessions": {"files": ("sessionstore.jsonlz4",), "directories": ("sessionstore-backups",)},
        }

    @staticmethod
    def category_patterns(strategy: str, categories: tuple[str, ...]) -> dict[str, dict[str, tuple[str, ...]]]:
        patterns = (
            TargetPatterns.chromium_category_patterns()
            if strategy == "chromium"
            else TargetPatterns.firefox_category_patterns()
        )
        return {category: patterns[category] for category in categories if category in patterns}

    @staticmethod
    def _flatten(patterns: dict[str, dict[str, tuple[str, ...]]], key: str) -> tuple[str, ...]:
        return tuple(pattern for category in BROWSER_CATEGORIES for pattern in patterns[category][key])
