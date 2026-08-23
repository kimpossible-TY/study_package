from __future__ import annotations

import unittest

try:
    from browser_cleaner.config import BrowserCatalog
    from browser_cleaner.models import CleanerOptions
    from browser_cleaner.textual_app import TextualCleanerApp
    from textual.widgets import Input, SelectionList
except ModuleNotFoundError as exc:  # pragma: no cover - depends on optional dev dependency
    if exc.name == "textual":
        raise unittest.SkipTest("Textual is not installed")
    raise


class TextualAppTests(unittest.IsolatedAsyncioTestCase):
    async def test_default_scope_is_preselected(self) -> None:
        app = TextualCleanerApp(
            BrowserCatalog(),
            CleanerOptions(False, False, False, "all"),
        )

        async with app.run_test(size=(100, 30)):
            selected = set(app.query_one("#category-list", SelectionList).selected)

        self.assertIn("site-data", selected)
        self.assertIn("developer-auth", selected)

    async def test_apply_requires_clean_before_running(self) -> None:
        app = TextualCleanerApp(
            BrowserCatalog(),
            CleanerOptions(False, False, False, "all"),
        )

        async with app.run_test(size=(100, 30)) as pilot:
            await pilot.click("#apply")
            await pilot.pause()

            self.assertTrue(app.query_one("#confirm-view").display)
            self.assertEqual(app.query_one("#confirm-input", Input).value, "")


if __name__ == "__main__":
    unittest.main()
