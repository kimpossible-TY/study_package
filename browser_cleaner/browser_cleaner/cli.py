from __future__ import annotations

import argparse

from .models import CleanerOptions


class ArgumentParserFactory:
    def build(self, browser_choices: tuple[str, ...]) -> argparse.ArgumentParser:
        parser = argparse.ArgumentParser(
            description="Clean Windows browser search, login, cookie, cache, and session data with TUI output."
        )
        parser.add_argument(
            "--apply",
            action="store_true",
            help="delete files instead of only showing what would be removed",
        )
        parser.add_argument(
            "--yes",
            action="store_true",
            help="skip the CLEAN confirmation prompt when used with --apply",
        )
        parser.add_argument(
            "--kill",
            action="store_true",
            help="force-close supported browsers before cleaning",
        )
        parser.add_argument(
            "--browser",
            choices=browser_choices + ("all",),
            default="all",
            help="limit cleaning to one browser",
        )
        return parser


class CleanerOptionsFactory:
    def __init__(self, parser_factory: ArgumentParserFactory) -> None:
        self._parser_factory = parser_factory

    def from_args(self, browser_choices: tuple[str, ...]) -> CleanerOptions:
        parser = self._parser_factory.build(browser_choices)
        args = parser.parse_args()
        return CleanerOptions(
            apply_changes=args.apply,
            skip_confirmation=args.yes,
            close_browsers=args.kill,
            browser_key=args.browser,
        )
