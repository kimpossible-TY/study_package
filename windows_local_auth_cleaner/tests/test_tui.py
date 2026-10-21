from __future__ import annotations

import io
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from browser_cleaner.models import CleanResult, CleanTarget
from browser_cleaner.tui import TerminalUI


class TuiTests(unittest.TestCase):
    def test_default_output_is_compact_and_does_not_emit_ansi_when_not_a_tty(self) -> None:
        output = io.StringIO()
        with redirect_stdout(output):
            ui = TerminalUI()
            ui.header("Cleaner", "dry run")
            ui.warning("No safe profiles found for Chrome.")
            ui.print_results([])

        rendered = output.getvalue()
        self.assertNotIn("\033[", rendered)
        self.assertNotIn("====", rendered)
        self.assertIn("No browser profile found: Chrome.", rendered)
        self.assertIn("No matching browser data found.", rendered)

    def test_default_results_show_counts_and_only_actionable_failures(self) -> None:
        target = CleanTarget("Chrome", "Default", Path("C:/Chrome/Default/History"), "history")
        output = io.StringIO()
        with redirect_stdout(output):
            ui = TerminalUI()
            ui.print_results(
                [
                    CleanResult(target, "dry-run", "would remove"),
                    CleanResult(target, "error", "file is locked"),
                ]
            )

        rendered = output.getvalue()
        self.assertIn("2 targets", rendered)
        self.assertIn("1 dry-run", rendered)
        self.assertIn("error · Chrome / Default · file is locked", rendered)
        self.assertNotIn("C:/Chrome/Default/History", rendered)

    def test_verbose_mode_keeps_detailed_result_path(self) -> None:
        target = CleanTarget("Chrome", "Default", Path("C:/Chrome/Default/History"), "history")
        output = io.StringIO()
        with redirect_stdout(output):
            ui = TerminalUI(verbose=True)
            ui.print_results([CleanResult(target, "dry-run", "would remove")])

        self.assertIn(str(target.path), output.getvalue())

    def test_legacy_windows_output_does_not_fail_on_unicode_guidance(self) -> None:
        buffer = io.BytesIO()
        output = io.TextIOWrapper(buffer, encoding="cp1252")
        with redirect_stdout(output):
            ui = TerminalUI()
            ui.print_vscode_manual_guidance()
            ui.success("Finished.")
        output.flush()
        self.assertIn(b"Finished.", buffer.getvalue())
        output.close()


if __name__ == "__main__":
    unittest.main()
