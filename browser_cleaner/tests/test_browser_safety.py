from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

from browser_cleaner.cleaning import PathCleaner
from browser_cleaner.models import BrowserDefinition, CleanTarget
from browser_cleaner.profiles import ProfileFinder


class BrowserSafetyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def test_category_selection_keeps_other_browser_data(self) -> None:
        profile = self.root / "Default"
        profile.mkdir()
        for name in ("History", "Login Data", "Cookies", "Web Data"):
            (profile / name).write_text("fixture", encoding="utf-8")
        (profile / "Cache").mkdir()
        (profile / "Local Storage").mkdir()
        browser = BrowserDefinition(
            "fixture", "Fixture Browser", (), (self.root,), (), (), "chromium"
        )

        targets = ProfileFinder().find_targets(browser, profile, ("site-data",))

        self.assertEqual([target.path.name for target in targets], ["Cookies", "Local Storage"])
        self.assertTrue((profile / "History").exists())
        self.assertTrue((profile / "Login Data").exists())

    def test_dry_run_and_apply_are_idempotent_in_a_temporary_profile(self) -> None:
        profile = self.root / "Default"
        profile.mkdir()
        cookies = profile / "Cookies"
        cookies.write_text("fixture", encoding="utf-8")
        target = CleanTarget("Fixture Browser", "Default", cookies, "site-data", profile)
        cleaner = PathCleaner()

        dry_run = cleaner.clean(target, apply_changes=False)
        self.assertEqual(dry_run.status, "dry-run")
        self.assertTrue(cookies.exists())
        self.assertEqual(cleaner.clean(target, apply_changes=True).status, "removed")
        self.assertFalse(cookies.exists())
        self.assertEqual(cleaner.clean(target, apply_changes=True).status, "missing")

    def test_outside_root_and_symlink_are_rejected(self) -> None:
        profile = self.root / "Default"
        profile.mkdir()
        outside = self.root / "outside"
        outside.write_text("fixture", encoding="utf-8")
        cleaner = PathCleaner()

        outside_result = cleaner.clean(
            CleanTarget("Fixture Browser", "Default", outside, "site-data", profile), True
        )
        self.assertEqual(outside_result.status, "error")
        self.assertTrue(outside.exists())
        traversal = profile / ".." / "outside"
        traversal_result = cleaner.clean(
            CleanTarget("Fixture Browser", "Default", traversal, "site-data", profile), True
        )
        self.assertEqual(traversal_result.status, "error")
        self.assertTrue(outside.exists())

        link = profile / "linked-cookies"
        try:
            link.symlink_to(outside)
        except (NotImplementedError, OSError) as exc:
            self.skipTest(f"symlink creation unavailable: {exc}")
        link_result = cleaner.clean(CleanTarget("Fixture Browser", "Default", link, "site-data", profile), True)
        self.assertEqual(link_result.status, "error")
        self.assertTrue(outside.exists())

    def test_reparse_like_child_refuses_tree_before_deletion(self) -> None:
        profile = self.root / "Default"
        cache = profile / "Cache"
        cache.mkdir(parents=True)
        external = self.root / "external"
        external.mkdir()
        (external / "keep").write_text("fixture", encoding="utf-8")
        try:
            (cache / "link").symlink_to(external, target_is_directory=True)
        except (NotImplementedError, OSError) as exc:
            self.skipTest(f"symlink creation unavailable: {exc}")

        result = PathCleaner().clean(CleanTarget("Fixture Browser", "Default", cache, "cache", profile), True)

        self.assertEqual(result.status, "error")
        self.assertTrue(cache.exists())
        self.assertTrue((external / "keep").exists())


if __name__ == "__main__":
    unittest.main()
