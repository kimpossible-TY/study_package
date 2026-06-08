#!/usr/bin/env python3
"""Entry point for the browser cleaner application.

This script launches the Windows Browser Privacy Cleaner, which detects running
browser processes, scans for profile directories, and cleans user history, cache,
cookies, and session data.
"""

from browser_cleaner.app import BrowserCleanerApp


if __name__ == "__main__":
    raise SystemExit(BrowserCleanerApp.from_command_line().run())

