"""Command-line interface and argument parsing.

Provides factories to construct the ArgumentParser and map incoming CLI arguments
to the structured CleanerOptions model.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from .models import ALL_CATEGORIES, CleanerOptions


class ArgumentParserFactory:
    """Factory for generating the command-line argument parser."""

    def build(self, browser_choices: tuple[str, ...]) -> argparse.ArgumentParser:
        """Constructs and configures the ArgumentParser object.

        Args:
            browser_choices: Valid keys of browsers currently supported/configured.

        Returns:
            An configured instance of argparse.ArgumentParser.
        """
        parser = argparse.ArgumentParser(
            description="Safely review or remove local Windows browser and developer-authentication evidence. With no selectors, use this as a two-command unified review/apply tool."
        )
        parser.add_argument(
            "--apply",
            action="store_true",
            help="apply the default unified cleanup or explicitly selected cleanup instead of only reviewing it",
        )
        parser.add_argument(
            "--yes",
            action="store_true",
            help="skip only the ordinary CLEAN confirmation (never sensitive profile or credential prompts)",
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
        parser.add_argument(
            "--category",
            action="append",
            choices=ALL_CATEGORIES,
            dest="categories",
            help="select one category; repeat for multiple categories",
        )
        parser.add_argument(
            "--audit-accounts",
            action="store_true",
            help="report secret-free local account and authentication evidence without changing data",
        )
        parser.add_argument(
            "--verify",
            action="store_true",
            help="run non-destructive local verification checks",
        )
        parser.add_argument(
            "--verbose",
            action="store_true",
            help="show detailed scan progress, paths, and result tables",
        )
        parser.add_argument(
            "--wipe-vscode-profile",
            action="store_true",
            help="remove complete local VS Code Stable and Insiders user-data profiles; requires --apply and a strong prompt",
        )
        parser.add_argument(
            "--backup-vscode-profile",
            type=Path,
            metavar="DIRECTORY",
            help="export only VS Code settings, keybindings, snippets, and extension names to a new directory",
        )
        parser.add_argument(
            "--wipe-browser-profile",
            "--full-profile",
            action="append",
            type=Path,
            dest="browser_profile_paths",
            metavar="PROFILE_PATH",
            help="remove one exact discovered browser profile; requires --apply and a path-specific strong prompt",
        )
        return parser


class CleanerOptionsFactory:
    """Factory to map command-line arguments to structured CleanerOptions."""

    def __init__(self, parser_factory: ArgumentParserFactory) -> None:
        """Initializes the factory with an argument parser builder.

        Args:
            parser_factory: The factory used to construct the parser.
        """
        self._parser_factory = parser_factory

    def from_args(self, browser_choices: tuple[str, ...]) -> CleanerOptions:
        """Parses the CLI args and constructs a CleanerOptions configuration instance.

        Args:
            browser_choices: Valid browser keys for choice validation.

        Returns:
            An instance of CleanerOptions.
        """
        parser = self._parser_factory.build(browser_choices)
        args = parser.parse_args()
        return CleanerOptions(
            apply_changes=args.apply,
            skip_confirmation=args.yes,
            close_browsers=args.kill,
            browser_key=args.browser,
            categories=tuple(args.categories or ()),
            audit_accounts=args.audit_accounts,
            verify=args.verify,
            wipe_vscode_profile=args.wipe_vscode_profile,
            vscode_backup=args.backup_vscode_profile,
            browser_profile_paths=tuple(args.browser_profile_paths or ()),
            verbose=args.verbose,
        )
