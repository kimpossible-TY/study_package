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
            description="Safely remove or review local Windows browser and developer-authentication evidence. By default, executes one-shot cleanup without prompts."
        )
        parser.add_argument(
            "--dry-run",
            action="store_true",
            help="review cleanup targets and account audit without deleting anything",
        )
        parser.add_argument(
            "--one-shot",
            action="store_true",
            help="run one-shot cleanup immediately without prompts (default)",
        )
        parser.add_argument(
            "--apply",
            action="store_true",
            help="apply cleanup changes (default unless --dry-run is specified)",
        )
        parser.add_argument(
            "--prompt",
            action="store_true",
            help="require confirmation prompt before deleting (disables default one-shot confirmation)",
        )
        parser.add_argument(
            "--yes",
            action="store_true",
            help="skip confirmation prompts (default in one-shot mode)",
        )
        parser.add_argument(
            "--kill",
            action="store_true",
            help="force-close supported browsers before cleaning (enabled by default when applying changes)",
        )
        parser.add_argument(
            "--no-kill",
            "--keep-browsers",
            action="store_true",
            dest="keep_browsers",
            help="do not automatically close running browsers before cleaning",
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
            "--interactive",
            "--tui",
            action="store_true",
            dest="interactive",
            help="launch the interactive Textual interface",
        )
        parser.add_argument(
            "--no-tui",
            action="store_true",
            help="force the plain command-line interface",
        )
        parser.add_argument(
            "--wipe-vscode-profile",
            action="store_true",
            help="remove complete local VS Code Stable and Insiders user-data profiles; requires confirmation unless in one-shot mode",
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
            help="remove one exact discovered browser profile",
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

    def from_args(
        self, browser_choices: tuple[str, ...], args: list[str] | None = None
    ) -> CleanerOptions:
        """Parses the CLI args and constructs a CleanerOptions configuration instance.

        Args:
            browser_choices: Valid browser keys for choice validation.
            args: Optional list of argument strings; defaults to sys.argv[1:].

        Returns:
            An instance of CleanerOptions.
        """
        parser = self._parser_factory.build(browser_choices)
        parsed = parser.parse_args(args)

        if parsed.dry_run:
            apply_changes = False
            skip_confirmation = False
            close_browsers = False
            one_shot = False
        elif parsed.interactive:
            apply_changes = parsed.apply
            skip_confirmation = parsed.yes
            close_browsers = parsed.kill or not parsed.keep_browsers
            one_shot = False
        else:
            # Default is one-shot cleanup
            apply_changes = True
            skip_confirmation = not parsed.prompt
            close_browsers = not parsed.keep_browsers
            one_shot = True

        return CleanerOptions(
            apply_changes=apply_changes,
            skip_confirmation=skip_confirmation,
            close_browsers=close_browsers,
            browser_key=parsed.browser,
            categories=tuple(parsed.categories or ()),
            audit_accounts=parsed.audit_accounts,
            verify=parsed.verify,
            wipe_vscode_profile=parsed.wipe_vscode_profile,
            vscode_backup=parsed.backup_vscode_profile,
            browser_profile_paths=tuple(parsed.browser_profile_paths or ()),
            verbose=parsed.verbose,
            interactive=parsed.interactive,
            no_tui=parsed.no_tui,
            one_shot=one_shot,
        )
