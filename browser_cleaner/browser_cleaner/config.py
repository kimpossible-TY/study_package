"""Configuration catalog and target search patterns.

Defines the directory roots, process names, and file/directory name patterns
used to scan and clean each supported browser (Chrome, Edge, Firefox, Brave).
"""

from __future__ import annotations

import os
from pathlib import Path

from .models import BrowserDefinition


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
        local_app_data = Path(os.environ.get("LOCALAPPDATA", ""))
        app_data = Path(os.environ.get("APPDATA", ""))
        chromium_files = TargetPatterns.chromium_files()
        chromium_directories = TargetPatterns.chromium_directories()

        return [
            BrowserDefinition(
                key="chrome",
                display_name="Google Chrome",
                process_names=("chrome.exe",),
                profile_roots=(local_app_data / "Google" / "Chrome" / "User Data",),
                file_patterns=chromium_files,
                directory_patterns=chromium_directories,
                profile_strategy="chromium",
            ),
            BrowserDefinition(
                key="edge",
                display_name="Microsoft Edge",
                process_names=("msedge.exe",),
                profile_roots=(local_app_data / "Microsoft" / "Edge" / "User Data",),
                file_patterns=chromium_files,
                directory_patterns=chromium_directories,
                profile_strategy="chromium",
            ),
            BrowserDefinition(
                key="firefox",
                display_name="Mozilla Firefox",
                process_names=("firefox.exe",),
                profile_roots=(app_data / "Mozilla" / "Firefox" / "Profiles",),
                file_patterns=TargetPatterns.firefox_files(),
                directory_patterns=TargetPatterns.firefox_directories(),
                profile_strategy="firefox",
            ),
            BrowserDefinition(
                key="brave",
                display_name="Brave",
                process_names=("brave.exe",),
                profile_roots=(local_app_data / "BraveSoftware" / "Brave-Browser" / "User Data",),
                file_patterns=chromium_files,
                directory_patterns=chromium_directories,
                profile_strategy="chromium",
            ),
        ]


class TargetPatterns:
    """Provides file and directory search patterns for different browser families."""

    @staticmethod
    def chromium_files() -> tuple[str, ...]:
        """Returns standard file names containing Chromium privacy data (e.g., history, cookies)."""
        return (

            "History",
            "History-journal",
            "Login Data",
            "Login Data-journal",
            "Cookies",
            "Cookies-journal",
            "Web Data",
            "Web Data-journal",
            "Network Action Predictor",
            "Network Action Predictor-journal",
            "Shortcuts",
            "Shortcuts-journal",
            "Top Sites",
            "Top Sites-journal",
            "Visited Links",
            "Favicons",
            "Favicons-journal",
        )

    @staticmethod
    def chromium_directories() -> tuple[str, ...]:
        """Returns directory names containing Chromium cache and temporary data."""
        return (

            "Cache",
            "Code Cache",
            "GPUCache",
            "Media Cache",
            "Session Storage",
            "Local Storage",
            "IndexedDB",
            "Service Worker",
            "File System",
            "blob_storage",
            "DawnCache",
            "GrShaderCache",
            "ShaderCache",
            "Sessions",
        )

    @staticmethod
    def firefox_files() -> tuple[str, ...]:
        """Returns standard file names containing Firefox SQLite databases and jsonlz4 session files."""
        return (

            "places.sqlite",
            "places.sqlite-wal",
            "places.sqlite-shm",
            "cookies.sqlite",
            "cookies.sqlite-wal",
            "cookies.sqlite-shm",
            "formhistory.sqlite",
            "formhistory.sqlite-wal",
            "formhistory.sqlite-shm",
            "signons.sqlite",
            "key4.db",
            "logins.json",
            "sessionstore.jsonlz4",
            "favicons.sqlite",
            "favicons.sqlite-wal",
            "favicons.sqlite-shm",
            "content-prefs.sqlite",
            "content-prefs.sqlite-wal",
            "content-prefs.sqlite-shm",
        )

    @staticmethod
    def firefox_directories() -> tuple[str, ...]:
        """Returns directory names containing Firefox cache and session backup data."""
        return (

            "cache2",
            "startupCache",
            "sessionstore-backups",
            "storage",
            "datareporting",
            "jumpListCache",
        )
