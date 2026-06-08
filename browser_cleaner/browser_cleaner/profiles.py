from __future__ import annotations

from pathlib import Path

from .models import BrowserDefinition, CleanTarget


class ProfileFinder:
    def find_profiles(self, browser: BrowserDefinition) -> list[Path]:
        profiles: list[Path] = []
        for root in browser.profile_roots:
            if not root.exists():
                continue
            profiles.extend(self._profiles_in_root(browser, root))
        return sorted(profiles, key=lambda path: path.name.lower())

    def find_targets(self, browser: BrowserDefinition, profile: Path) -> list[CleanTarget]:
        targets: list[CleanTarget] = []
        for pattern in browser.file_patterns:
            self._append_if_exists(targets, browser, profile, profile / pattern)
        for pattern in browser.directory_patterns:
            self._append_if_exists(targets, browser, profile, profile / pattern)
        return sorted(targets, key=lambda target: (target.path.is_dir(), str(target.path).lower()))

    def _profiles_in_root(self, browser: BrowserDefinition, root: Path) -> list[Path]:
        if browser.profile_strategy == "firefox":
            return [path for path in root.iterdir() if path.is_dir()]
        return [path for path in root.iterdir() if path.is_dir() and self._is_chromium_profile(path)]

    def _is_chromium_profile(self, path: Path) -> bool:
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
        if path.exists():
            targets.append(
                CleanTarget(
                    browser_name=browser.display_name,
                    profile_name=profile.name,
                    path=path,
                )
            )
