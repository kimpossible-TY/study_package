#!/usr/bin/env python3
"""Entry point for the browser cleaner application."""

from __future__ import annotations

import sys

from browser_cleaner.app import BrowserCleanerApp
from browser_cleaner.cli import ArgumentParserFactory, CleanerOptionsFactory
from browser_cleaner.config import BrowserCatalog


def main() -> int:
    catalog = BrowserCatalog()
    options = CleanerOptionsFactory(ArgumentParserFactory()).from_args(catalog.keys())
    wants_tui = options.interactive
    if not wants_tui:
        return BrowserCleanerApp.from_options(options, catalog).run()

    try:
        from browser_cleaner.textual_app import TextualCleanerApp
    except ModuleNotFoundError as exc:
        if exc.name != "textual":
            raise
        print("Textual is not installed. Run: python -m pip install -r requirements.txt", file=sys.stderr)
        print("Or use --no-tui for the plain command-line interface.", file=sys.stderr)
        return 2

    return TextualCleanerApp(catalog, options).run()


if __name__ == "__main__":
    raise SystemExit(main())
