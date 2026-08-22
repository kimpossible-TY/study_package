from __future__ import annotations

import unittest
from pathlib import Path

from browser_cleaner.cli import ArgumentParserFactory


class CliTests(unittest.TestCase):
    def test_category_selection_and_full_profile_path_are_explicit(self) -> None:
        parser = ArgumentParserFactory().build(("chrome",))

        args = parser.parse_args(
            [
                "--browser", "chrome",
                "--category", "site-data",
                "--category", "sessions",
                "--wipe-browser-profile", "C:/Users/example/AppData/Local/Google/Chrome/User Data/Default",
                "--apply",
                "--yes",
            ]
        )

        self.assertEqual(args.browser, "chrome")
        self.assertEqual(args.categories, ["site-data", "sessions"])
        self.assertEqual(args.browser_profile_paths, [Path("C:/Users/example/AppData/Local/Google/Chrome/User Data/Default")])
        self.assertTrue(args.apply)
        self.assertTrue(args.yes)


if __name__ == "__main__":
    unittest.main()
