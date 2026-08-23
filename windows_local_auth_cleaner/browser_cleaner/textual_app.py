"""Interactive Textual front-end for the browser cleaner.

The existing service layer remains responsible for discovery, safety checks,
deletion, and verification. This module only collects intent and renders the
progress/results screen.
"""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
from threading import Event
from typing import Any

from textual import on
from textual.app import App, ComposeResult
from textual.containers import Container, Horizontal, Vertical
from textual.widgets import Button, Footer, Header, Input, Label, RichLog, Select, SelectionList, Static

from .app import BrowserCleanerApp
from .config import BrowserCatalog
from .models import (
    BROWSER_CATEGORIES,
    AccountAuditEntry,
    ActionResult,
    CleanResult,
    CleanerOptions,
    VerificationEntry,
)
from .tui import TerminalUI


CATEGORY_LABELS = {
    "sessions": "Sessions",
    "history": "History & search",
    "passwords": "Saved passwords",
    "autofill": "Autofill",
    "site-data": "Cookies & site data",
    "cache": "Cache",
    "developer-auth": "Developer authentication",
}


@dataclass
class _PendingPrompt:
    event: Event
    phrase: str
    result: dict[str, bool]


class TextualCleanerApp(App[int]):
    """Collect cleanup scope and run the existing cleaner inside a TUI."""

    TITLE = "Windows Local Authentication Cleaner"
    CSS = """
    Screen {
        layers: base modal;
        background: $surface;
    }

    Header {
        background: $panel;
        color: $text;
    }

    Footer {
        background: $panel;
    }

    #selection-view, #run-view {
        width: 100%;
        height: 1fr;
        padding: 1 2;
    }

    #intro {
        height: auto;
        margin-bottom: 1;
        color: $text-muted;
    }

    #selectors {
        height: 1fr;
        min-height: 12;
    }

    .panel {
        height: 100%;
        padding: 1;
        border: round $primary-darken-2;
        background: $panel;
    }

    #browser-panel {
        width: 1fr;
        margin-right: 1;
    }

    #category-panel {
        width: 2fr;
    }

    .panel-title {
        height: auto;
        margin-bottom: 1;
        color: $text-muted;
    }

    Select {
        width: 100%;
    }

    SelectionList {
        height: 1fr;
        border: round $primary-darken-2;
    }

    #scope-note {
        height: auto;
        margin-top: 1;
        color: $text-muted;
    }

    #selection-actions, #run-actions {
        height: auto;
        margin-top: 1;
        align-horizontal: right;
    }

    Button {
        margin-left: 1;
    }

    #run-status {
        height: auto;
        margin-bottom: 1;
        color: $text-muted;
    }

    #run-log {
        height: 1fr;
        border: round $primary-darken-2;
        padding: 1;
        background: $panel;
    }

    #confirm-view, #prompt-view {
        width: 70%;
        height: auto;
        padding: 2;
        border: double $warning;
        background: $panel;
        align: center middle;
    }

    #confirm-view {
        margin: 4 0;
    }

    #prompt-view {
        layer: modal;
        align: center middle;
    }

    #confirm-title, #prompt-title {
        height: auto;
        margin-bottom: 1;
        text-style: bold;
    }

    #confirm-copy, #prompt-copy, #prompt-error {
        height: auto;
        margin-bottom: 1;
        color: $text-muted;
    }

    #prompt-error {
        color: $error;
    }

    .hidden {
        display: none;
    }
    """
    BINDINGS = [("ctrl+q", "quit", "Quit")]

    def __init__(self, catalog: BrowserCatalog, base_options: CleanerOptions) -> None:
        super().__init__()
        self.catalog = catalog
        self.base_options = base_options
        self._pending_prompt: _PendingPrompt | None = None
        self._pending_scope: tuple[str, tuple[str, ...]] = ("all", ())

    def compose(self) -> ComposeResult:
        yield Header()
        with Container(id="selection-view"):
            yield Static(
                "Choose what to inspect. Preview is always safe; Apply asks for an explicit confirmation.",
                id="intro",
            )
            with Horizontal(id="selectors"):
                with Vertical(id="browser-panel", classes="panel"):
                    yield Label("Browser", classes="panel-title")
                    yield Select(self._browser_options(), value=self._initial_browser(), id="browser-select")
                with Vertical(id="category-panel", classes="panel"):
                    yield Label("Data to clean", classes="panel-title")
                    yield SelectionList[str](*self._category_options(), id="category-list")
                    yield Static(
                        "Space: toggle · ↑/↓: move · Tab: next control",
                        id="scope-note",
                    )
            with Horizontal(id="selection-actions"):
                yield Button("Preview", id="preview", variant="primary")
                yield Button("Apply…", id="apply", variant="warning")
                yield Button("Quit", id="quit")

        with Container(id="confirm-view", classes="hidden"):
            yield Static("Confirm local cleanup", id="confirm-title")
            yield Static(
                "This will remove the selected browser data from this computer.\nType CLEAN exactly to continue.",
                id="confirm-copy",
            )
            yield Input(placeholder="Type CLEAN", id="confirm-input")
            with Horizontal(id="confirm-actions"):
                yield Button("Continue", id="confirm-continue", variant="warning")
                yield Button("Back", id="confirm-back")

        with Container(id="run-view", classes="hidden"):
            yield Static("Preparing…", id="run-status")
            yield RichLog(id="run-log", markup=False, wrap=True, auto_scroll=True)
            with Horizontal(id="run-actions"):
                yield Button("Close", id="close", disabled=True)

        with Container(id="prompt-view", classes="hidden"):
            yield Static("Sensitive confirmation", id="prompt-title")
            yield Static("", id="prompt-copy")
            yield Input(placeholder="Type the phrase exactly", id="prompt-input")
            yield Static("", id="prompt-error")
            with Horizontal(id="prompt-actions"):
                yield Button("Confirm", id="prompt-confirm", variant="warning")
                yield Button("Decline", id="prompt-decline")
        yield Footer()

    def on_mount(self) -> None:
        self.query_one("#category-list", SelectionList).focus()

    @on(Button.Pressed)
    def on_button_pressed(self, event: Button.Pressed) -> None:
        button_id = event.button.id
        if button_id == "preview":
            self._begin_preview()
        elif button_id == "apply":
            self._begin_apply()
        elif button_id == "confirm-continue":
            self._confirm_apply()
        elif button_id == "confirm-back":
            self._show_selection()
        elif button_id == "prompt-confirm":
            self._resolve_prompt(True)
        elif button_id == "prompt-decline":
            self._resolve_prompt(False)
        elif button_id in {"quit", "close"}:
            self.exit(0)

    def _browser_options(self) -> list[tuple[str, str]]:
        return [("All supported browsers", "all")] + [
            (browser.display_name, browser.key) for browser in self.catalog.all()
        ]

    def _initial_browser(self) -> str:
        browser_key = self.base_options.browser_key
        return browser_key if browser_key in {key for _, key in self._browser_options()} else "all"

    def _category_options(self) -> list[tuple[str, str, bool]]:
        selected = set(self.base_options.categories or (*BROWSER_CATEGORIES, "developer-auth"))
        return [
            (CATEGORY_LABELS[category], category, category in selected)
            for category in (*BROWSER_CATEGORIES, "developer-auth")
        ]

    def _selected_scope(self) -> tuple[str, tuple[str, ...]] | None:
        browser_value = self.query_one("#browser-select", Select).value
        categories = tuple(self.query_one("#category-list", SelectionList).selected)
        if not isinstance(browser_value, str):
            self.notify("Choose a browser.", severity="error")
            return None
        if not categories:
            self.notify("Choose at least one data category.", severity="error")
            return None
        return browser_value, categories

    def _begin_preview(self) -> None:
        scope = self._selected_scope()
        if scope is not None:
            self._start_run(False, *scope)

    def _begin_apply(self) -> None:
        scope = self._selected_scope()
        if scope is None:
            return
        if self.base_options.skip_confirmation:
            self._start_run(True, *scope)
            return
        self._pending_scope = scope
        self._show_apply_confirmation()

    def _show_apply_confirmation(self) -> None:
        self.query_one("#selection-view").add_class("hidden")
        self.query_one("#confirm-view").remove_class("hidden")
        confirm_input = self.query_one("#confirm-input", Input)
        confirm_input.value = ""
        confirm_input.focus()

    def _confirm_apply(self) -> None:
        if self.query_one("#confirm-input", Input).value.strip() != "CLEAN":
            self.notify("Type CLEAN exactly to continue.", severity="error")
            return
        browser_key, categories = self._pending_scope
        self._start_run(True, browser_key, categories)

    def _show_selection(self) -> None:
        self.query_one("#confirm-view").add_class("hidden")
        self.query_one("#selection-view").remove_class("hidden")
        self.query_one("#category-list", SelectionList).focus()

    def _start_run(self, apply_changes: bool, browser_key: str, categories: tuple[str, ...]) -> None:
        self.query_one("#selection-view").add_class("hidden")
        self.query_one("#confirm-view").add_class("hidden")
        self.query_one("#run-view").remove_class("hidden")
        self.query_one("#run-log", RichLog).clear()
        self.query_one("#run-status", Static).update("Scanning selected targets…")
        self.query_one("#close", Button).disabled = True
        options = self._build_options(apply_changes, browser_key, categories)
        self.run_worker(lambda: self._run_cleaner(options), thread=True, exclusive=True)

    def _build_options(self, apply_changes: bool, browser_key: str, categories: tuple[str, ...]) -> CleanerOptions:
        return CleanerOptions(
            apply_changes=apply_changes,
            skip_confirmation=apply_changes or self.base_options.skip_confirmation,
            close_browsers=self.base_options.close_browsers,
            browser_key=browser_key,
            categories=categories,
            audit_accounts=self.base_options.audit_accounts,
            verify=self.base_options.verify,
            wipe_vscode_profile=self.base_options.wipe_vscode_profile,
            vscode_backup=self.base_options.vscode_backup,
            browser_profile_paths=self.base_options.browser_profile_paths,
            verbose=False,
            interactive=False,
            no_tui=True,
        )

    def _run_cleaner(self, options: CleanerOptions) -> int:
        bridge = TextualUIBridge(self)
        try:
            result = BrowserCleanerApp.from_options(options, self.catalog, ui=bridge).run()
        except Exception as exc:  # pragma: no cover - defensive UI boundary
            bridge.error(f"Unexpected failure: {exc}")
            result = 1
        self.call_from_thread(self._finish_run, result)
        return result

    def _finish_run(self, result: int) -> None:
        message = "Finished successfully." if result == 0 else f"Finished with exit code {result}."
        self.query_one("#run-status", Static).update(message)
        self.query_one("#close", Button).disabled = False

    def add_log(self, message: str, style: str | None = None) -> None:
        from rich.text import Text

        renderable: Any = Text(message, style=style) if style else message
        self.query_one("#run-log", RichLog).write(renderable)

    def set_run_status(self, message: str) -> None:
        self.query_one("#run-status", Static).update(message)

    def request_prompt(self, subject: str, phrase: str, event: Event, result: dict[str, bool]) -> None:
        self._pending_prompt = _PendingPrompt(event, phrase, result)
        self.query_one("#prompt-title", Static).update("Sensitive confirmation")
        self.query_one("#prompt-copy", Static).update(
            f"{subject}\nThis operation may remove more than a website session.\nType exactly: {phrase}"
        )
        self.query_one("#prompt-error", Static).update("")
        self.query_one("#prompt-input", Input).value = ""
        self.query_one("#prompt-view").remove_class("hidden")
        self.query_one("#prompt-input", Input).focus()

    def _resolve_prompt(self, accepted: bool) -> None:
        pending = self._pending_prompt
        if pending is None:
            return
        if accepted and self.query_one("#prompt-input", Input).value.strip() != pending.phrase:
            self.query_one("#prompt-error", Static).update("The phrase does not match.")
            return
        pending.result["accepted"] = accepted
        pending.event.set()
        self._pending_prompt = None
        self.query_one("#prompt-view").add_class("hidden")


class TextualUIBridge(TerminalUI):
    """Adapt the existing synchronous UI contract to Textual's main thread."""

    def __init__(self, app: TextualCleanerApp) -> None:
        super().__init__(verbose=False)
        self._app = app

    def setup(self) -> None:
        return None

    def header(self, title: str, subtitle: str) -> None:
        self._app.call_from_thread(self._app.set_run_status, subtitle)

    def step(self, message: str) -> None:
        self._log(f"• {message}", "cyan")

    def warning(self, message: str) -> None:
        self._log(f"! {message}", "yellow")

    def success(self, message: str) -> None:
        self._log(f"✓ {message}", "green")

    def error(self, message: str) -> None:
        self._log(f"× {message}", "red")

    def confirm_apply(self, skip_prompt: bool) -> bool:
        if skip_prompt:
            return True
        return self._prompt("Browser cleanup", "CLEAN")

    def confirm_sensitive(self, subject: str, phrase: str) -> bool:
        return self._prompt(subject, phrase)

    def print_targets(self, targets) -> None:
        self._log("Selected browser targets", "bold")
        if not targets:
            self._log("No matching selected browser paths were found.", "yellow")
        for target in targets:
            self._log(f"[{target.category}] {target.browser_name} / {target.profile_name} / {target.path}")

    def print_results(self, results: list[CleanResult]) -> None:
        self._log("Browser cleanup", "bold")
        if not results:
            self._log("No matching browser data found.", "dim")
            return
        counts = Counter(result.status for result in results)
        summary = " · ".join(f"{count} {status}" for status, count in counts.items())
        label = "target" if len(results) == 1 else "targets"
        self._log(f"{len(results)} {label} · {summary}")
        for result in results:
            if result.status in {"locked", "error"}:
                self._log(
                    f"{result.status} · {result.target.browser_name} / {result.target.profile_name} · {result.details}",
                    "red",
                )

    def print_summary(self, results: list[CleanResult], apply_changes: bool) -> None:
        self._log(
            "APPLY · local changes applied" if apply_changes else "DRY RUN · nothing changed",
            "green" if apply_changes else "yellow",
        )

    def print_audit(self, entries: list[AccountAuditEntry]) -> None:
        self._log("Local account check", "bold")
        active = [
            entry
            for entry in entries
            if entry.automatic_removal == "yes"
            and entry.login_status not in {"unsupported", "not configured", "no local evidence found", "unknown"}
        ]
        manual_count = sum(entry.manual_action == "yes" for entry in entries)
        if active:
            for entry in active:
                self._log(f"{entry.provider} · {entry.account_name} · local sign-out available")
        else:
            self._log("No active local sign-out candidates found.", "dim")
        if manual_count:
            self._log(f"{manual_count} manual checks available.", "dim")

    def print_action_results(self, results: list[ActionResult]) -> None:
        if not results:
            return
        self._log("Developer authentication", "bold")
        for result in results:
            style = "red" if result.status in {"error", "locked", "manual"} else None
            self._log(f"{result.status} · {result.provider} · {result.subject}", style)

    def print_verification(self, entries: list[VerificationEntry]) -> None:
        if not entries:
            return
        counts = Counter(entry.status for entry in entries)
        self._log("Verification", "bold")
        self._log(" · ".join(f"{count} {status}" for status, count in counts.items()))

    def print_vscode_manual_guidance(self) -> None:
        self._log("VS Code account and sync sign-out is manual via Accounts → Settings Sync.", "dim")

    def print_browser_sync_guidance(self) -> None:
        self._log("Turn off browser sync and sign out in each profile before deleting site data.", "dim")

    def print_remote_revocation_guidance(self) -> None:
        self._log("Remote follow-up: review active sessions, OAuth apps, and tokens in provider accounts.", "dim")

    def print_windows_profile_guidance(self) -> None:
        self._log("Device handoff: remove the old OS user profile separately after backing up needed files.", "dim")

    def _log(self, message: str, style: str | None = None) -> None:
        self._app.call_from_thread(self._app.add_log, message, style)

    def _prompt(self, subject: str, phrase: str) -> bool:
        event = Event()
        result: dict[str, bool] = {"accepted": False}
        self._app.call_from_thread(self._app.request_prompt, subject, phrase, event, result)
        event.wait()
        return result["accepted"]
