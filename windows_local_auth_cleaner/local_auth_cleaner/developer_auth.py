"""Secret-safe discovery and removal of supported local developer authentication.

This module intentionally uses public CLIs for GitHub CLI, Git Credential
Manager, Microsoft Dev Tunnels, and VS Code Remote Tunnels. It never opens
VS Code authentication databases, browser databases, Credential Manager secret
blobs, or registry environment-variable values.
"""

from __future__ import annotations

import ctypes
import json
import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable

from .cleaning import PathCleaner
from .models import AccountAuditEntry, ActionResult, CleanTarget
from .powershell import PowerShellProfileManager


@dataclass(frozen=True)
class CommandResult:
    """A completed command result whose output is used only for status parsing."""

    returncode: int
    stdout: str
    stderr: str


class CommandRunner:
    """Small subprocess boundary that is straightforward to mock in tests."""

    def run(self, command: tuple[str, ...]) -> CommandResult | None:
        try:
            completed = subprocess.run(
                command,
                capture_output=True,
                text=True,
                check=False,
                encoding="utf-8",
                errors="replace",
            )
        except FileNotFoundError:
            return None
        return CommandResult(completed.returncode, completed.stdout, completed.stderr)


@dataclass(frozen=True)
class CredentialEntry:
    """Metadata returned by Credential Manager; it deliberately excludes blobs."""

    target_name: str
    username: str
    credential_type: int


class WindowsCredentialStore:
    """Current-user Credential Manager wrapper that never accesses secret blobs."""

    _ERROR_NOT_FOUND = 1168

    def is_supported(self) -> bool:
        return os.name == "nt"

    def list_supported_entries(self) -> list[CredentialEntry] | None:
        """Enumerate only approved developer-tool target metadata.

        ``CredEnumerateW`` exposes a credential blob, but this implementation
        deliberately copies only target name, username, and type from each
        record and never dereferences the blob pointer.
        """
        if not self.is_supported():
            return None

        from ctypes import wintypes

        class FILETIME(ctypes.Structure):
            _fields_ = [("dwLowDateTime", wintypes.DWORD), ("dwHighDateTime", wintypes.DWORD)]

        class CREDENTIAL_ATTRIBUTEW(ctypes.Structure):
            _fields_ = [
                ("Keyword", wintypes.LPWSTR),
                ("Flags", wintypes.DWORD),
                ("ValueSize", wintypes.DWORD),
                ("Value", ctypes.POINTER(ctypes.c_byte)),
            ]

        class CREDENTIALW(ctypes.Structure):
            _fields_ = [
                ("Flags", wintypes.DWORD),
                ("Type", wintypes.DWORD),
                ("TargetName", wintypes.LPWSTR),
                ("Comment", wintypes.LPWSTR),
                ("LastWritten", FILETIME),
                ("CredentialBlobSize", wintypes.DWORD),
                ("CredentialBlob", ctypes.POINTER(ctypes.c_byte)),
                ("Persist", wintypes.DWORD),
                ("AttributeCount", wintypes.DWORD),
                ("Attributes", ctypes.POINTER(CREDENTIAL_ATTRIBUTEW)),
                ("TargetAlias", wintypes.LPWSTR),
                ("UserName", wintypes.LPWSTR),
            ]

        credential_pointer = ctypes.POINTER(CREDENTIALW)
        credentials = ctypes.POINTER(credential_pointer)()
        count = wintypes.DWORD()
        advapi32 = ctypes.WinDLL("Advapi32.dll", use_last_error=True)
        enumerate_credentials = advapi32.CredEnumerateW
        enumerate_credentials.argtypes = [
            wintypes.LPWSTR,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(ctypes.POINTER(credential_pointer)),
        ]
        enumerate_credentials.restype = wintypes.BOOL
        if not enumerate_credentials(None, 0, ctypes.byref(count), ctypes.byref(credentials)):
            if ctypes.get_last_error() == self._ERROR_NOT_FOUND:
                return []
            return []

        entries: list[CredentialEntry] = []
        try:
            for index in range(count.value):
                credential = credentials[index].contents
                target_name = credential.TargetName or ""
                if self._is_supported_target(target_name):
                    entries.append(
                        CredentialEntry(
                            target_name=target_name,
                            username=credential.UserName or "(username not exposed)",
                            credential_type=int(credential.Type),
                        )
                    )
        finally:
            advapi32.CredFree(credentials)
        return entries

    def delete(self, entry: CredentialEntry) -> bool:
        """Delete one already allowlisted current-user credential by target and type."""
        if not self.is_supported():
            return False
        from ctypes import wintypes

        advapi32 = ctypes.WinDLL("Advapi32.dll", use_last_error=True)
        delete_credential = advapi32.CredDeleteW
        delete_credential.argtypes = [wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD]
        delete_credential.restype = wintypes.BOOL
        return bool(delete_credential(entry.target_name, entry.credential_type, 0))

    def _is_supported_target(self, target_name: str) -> bool:
        target = target_name.casefold()
        return any(
            marker in target
            for marker in (
                "github",
                "git credential manager",
                "vscode",
                "visual studio code",
                "azure",
            )
        )


class UserEnvironmentStore:
    """Read and remove *names* of current-user token variables without reading values."""

    static_allowlist = {
        "GH_TOKEN",
        "GITHUB_TOKEN",
        "GITHUB_ENTERPRISE_TOKEN",
        "AZURE_DEVOPS_EXT_PAT",
    }

    def list_token_names(self) -> set[str] | None:
        """List HKCU\\Environment value names using ``RegEnumValueW`` with no data buffer."""
        if os.name != "nt":
            return None
        import winreg
        from ctypes import wintypes

        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment", 0, winreg.KEY_READ) as key:
                value_count = winreg.QueryInfoKey(key)[1]
                advapi32 = ctypes.WinDLL("Advapi32.dll", use_last_error=True)
                enum_value = advapi32.RegEnumValueW
                enum_value.argtypes = [
                    wintypes.HKEY,
                    wintypes.DWORD,
                    wintypes.LPWSTR,
                    ctypes.POINTER(wintypes.DWORD),
                    ctypes.c_void_p,
                    ctypes.c_void_p,
                    ctypes.c_void_p,
                    ctypes.c_void_p,
                ]
                enum_value.restype = wintypes.LONG
                names: set[str] = set()
                for index in range(value_count):
                    name = ctypes.create_unicode_buffer(32_768)
                    name_size = wintypes.DWORD(len(name) - 1)
                    # Passing no data/type buffers is intentional: registry token
                    # values are never fetched into this process.
                    status = enum_value(
                        wintypes.HKEY(key.handle), index, name, ctypes.byref(name_size), None, None, None, None
                    )
                    if status == 0 and self._is_allowed(name.value):
                        names.add(name.value)
                return names
        except OSError:
            return set()

    def delete(self, name: str) -> bool:
        """Delete a single allowlisted user-scope environment variable by name only."""
        if os.name != "nt" or not self._is_allowed(name):
            return False
        import winreg

        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment", 0, winreg.KEY_SET_VALUE) as key:
                winreg.DeleteValue(key, name)
            return True
        except FileNotFoundError:
            return True
        except OSError:
            return False

    def audit_names(self) -> list[str] | None:
        names = self.list_token_names()
        if names is None:
            return None
        return sorted(self.static_allowlist | names, key=str.casefold)

    def _is_allowed(self, name: str) -> bool:
        return name.upper() in self.static_allowlist or name.upper().startswith("AZURE_")


class DeveloperAuthService:
    """Audits and removes only explicitly supported developer-authentication records."""

    TAILSCALE_PIPE_SOCKET = r"\\.\pipe\tailscale-dev"
    TAILSCALE_STATE_FILES = (
        "tailscaled.state",
        "tailscaled.pid",
        "mac-mirror-viewer.pid",
        "mac-vnc-tunnel.json",
    )

    def __init__(
        self,
        runner: CommandRunner | None = None,
        credential_store: WindowsCredentialStore | None = None,
        environment_store: UserEnvironmentStore | None = None,
        app_data: Path | None = None,
        code_commands: tuple[str, ...] = ("code", "code-insiders"),
        devtunnel_command: str | None = None,
        powershell_profiles: PowerShellProfileManager | None = None,
        tailscale_command: str | None = None,
        tailscale_state_dir: Path | None = None,
        mac_mirror_dir: Path | None = None,
    ) -> None:
        self._runner = runner or CommandRunner()
        self._credential_store = credential_store or WindowsCredentialStore()
        self._environment_store = environment_store or UserEnvironmentStore()
        app_data_value = os.environ.get("APPDATA")
        self._app_data = app_data if app_data is not None else (Path(app_data_value) if app_data_value else None)
        self._code_commands = code_commands
        self._devtunnel_command = devtunnel_command or self._find_devtunnel_command()
        self.powershell_profiles = powershell_profiles or PowerShellProfileManager()
        self._tailscale_command = tailscale_command or self._find_tailscale_command()
        self._tailscale_state_dir = tailscale_state_dir if tailscale_state_dir is not None else (Path.home() / ".tailscale")
        local_app_data = os.environ.get("LOCALAPPDATA")
        self._mac_mirror_dir = (
            mac_mirror_dir
            if mac_mirror_dir is not None
            else (Path(local_app_data) / "Programs" / "mac-mirror" if local_app_data else None)
        )

    def audit(self) -> list[AccountAuditEntry]:
        """Return a complete best-effort audit; one unavailable tool never hides others."""
        entries: list[AccountAuditEntry] = []
        entries.extend(self._audit_github_cli())
        entries.extend(self._audit_git_credential_manager())
        entries.extend(self._audit_vscode())
        entries.extend(self._audit_remote_tunnels())
        entries.extend(self._audit_devtunnel())
        entries.extend(self._audit_tailscale_mac_mirror())
        entries.extend(self._audit_credentials())
        entries.extend(self._audit_environment())
        entries.extend(self.powershell_profiles.audit())
        return entries

    def github_cli_accounts(self) -> list[str] | None:
        outcome = self._runner.run(("gh", "auth", "status"))
        if outcome is None:
            return None
        return self._parse_gh_accounts(outcome.stdout + "\n" + outcome.stderr)

    def git_credential_manager_accounts(self) -> list[str] | None:
        outcome = self._runner.run(("git", "credential-manager", "github", "list"))
        if outcome is None:
            return None
        if outcome.returncode != 0:
            return []
        return self._parse_gcm_accounts(outcome.stdout)

    def credential_entries(self) -> list[CredentialEntry] | None:
        """Return allowlisted Credential Manager metadata, never secret blobs."""
        return self._credential_store.list_supported_entries()

    def environment_token_names(self) -> set[str] | None:
        """Return allowlisted current-user token variable names, never values."""
        return self._environment_store.list_token_names()

    @property
    def code_commands(self) -> tuple[str, ...]:
        """VS Code CLI command variants checked by this service."""
        return self._code_commands

    def tunnel_status(self, command: str) -> str:
        outcome = self._runner.run((command, "tunnel", "status"))
        if outcome is None:
            return "unsupported"
        combined = (outcome.stdout + "\n" + outcome.stderr).casefold()
        if outcome.returncode != 0:
            return "unknown"
        if any(marker in combined for marker in ("not registered", "not logged in", "no tunnel")):
            return "not registered"
        return "possibly registered"

    @staticmethod
    def _find_devtunnel_command() -> str:
        command = shutil.which("devtunnel")
        if command:
            return command
        # The bootstrapper's per-user install can be found even when this
        # process was launched with an older PATH (for example from Explorer).
        local_app_data = os.environ.get("LOCALAPPDATA")
        if local_app_data:
            executable = Path(local_app_data) / "Programs" / "devtunnel" / "devtunnel.exe"
            if executable.is_file():
                return str(executable)
        return "devtunnel"

    @property
    def devtunnel_command(self) -> str:
        """The executable used for official Dev Tunnel login/logout commands."""
        return self._devtunnel_command

    def devtunnel_login_status(self) -> str:
        """Inspect CLI login status without displaying raw output or token values."""
        try:
            outcome = self._runner.run((self._devtunnel_command, "user", "show"))
        except OSError:
            return "unknown"
        if outcome is None:
            return "unsupported"
        combined = (outcome.stdout + "\n" + outcome.stderr).casefold()
        # Only explicit status messages count as evidence. An error, empty
        # output, or an unfamiliar/localized response must not imply sign-out.
        if outcome.returncode in (0, 1) and re.fullmatch(r"not logged in[.!]?", combined.strip()):
            return "not logged in"
        if outcome.returncode == 0 and re.search(r"^\s*logged in\b", combined, re.MULTILINE):
            return "logged in"
        return "unknown"

    @staticmethod
    def _find_tailscale_command() -> str:
        local_app_data = os.environ.get("LOCALAPPDATA")
        if local_app_data:
            executable = Path(local_app_data) / "Programs" / "tailscale" / "tailscale.exe"
            if executable.is_file():
                return str(executable)
        command = shutil.which("tailscale")
        if command:
            return command
        return "tailscale"

    @property
    def tailscale_command(self) -> str:
        """The executable used for Tailscale userspace status/logout commands."""
        return self._tailscale_command

    @property
    def tailscale_state_dir(self) -> Path:
        """Directory holding Tailscale userspace state and mac-mirror PID files."""
        return self._tailscale_state_dir

    @property
    def mac_mirror_prefs_path(self) -> Path | None:
        """Path to non-sensitive mac-mirror UI preferences, if LOCALAPPDATA is known."""
        if self._mac_mirror_dir is None:
            return None
        return self._mac_mirror_dir / "ui-prefs.json"

    def _query_tailscale_daemon_state(self) -> str | None:
        """Return Tailscale BackendState if the userspace daemon responds, else None."""
        try:
            outcome = self._runner.run((self._tailscale_command, f"--socket={self.TAILSCALE_PIPE_SOCKET}", "status", "--json"))
        except OSError:
            return None
        if outcome is None or not outcome.stdout.strip():
            return None
        try:
            data = json.loads(outcome.stdout)
        except json.JSONDecodeError:
            return None
        if isinstance(data, dict) and isinstance(data.get("BackendState"), str):
            return data["BackendState"]
        return None

    def tailscale_userspace_status(self) -> str:
        """Inspect Tailscale userspace session/state without exposing account details."""
        state_file = self._tailscale_state_dir / "tailscaled.state"
        has_state_file = state_file.exists() or state_file.is_symlink()
        backend = self._query_tailscale_daemon_state()
        if backend in {"Running", "Starting"}:
            return "logged in"
        if backend == "Stopped":
            return "stopped (state present)" if has_state_file else "stopped"
        if has_state_file:
            return "state file present"
        return "not logged in"

    def remove_supported(self, apply_changes: bool, confirm: Callable[[str, str], bool]) -> list[ActionResult]:
        """Remove supported records, requesting a strong confirmation for every item.

        ``confirm`` is called only for live changes.  The caller must not let
        ``--yes`` bypass it.
        """
        results: list[ActionResult] = []
        results.extend(self._remove_github_cli(apply_changes, confirm))
        results.extend(self._remove_gcm(apply_changes, confirm))
        results.extend(self._remove_tunnels(apply_changes, confirm))
        results.extend(self._remove_devtunnel(apply_changes, confirm))
        results.extend(self._remove_tailscale_mac_mirror(apply_changes, confirm))
        results.extend(self._remove_credentials(apply_changes, confirm))
        results.extend(self._remove_environment(apply_changes, confirm))
        results.extend(self.powershell_profiles.remove(apply_changes, confirm))
        return results

    def _audit_github_cli(self) -> list[AccountAuditEntry]:
        accounts = self.github_cli_accounts()
        if accounts is None:
            return [self._unsupported("GitHub CLI", "gh")]
        if not accounts:
            return [AccountAuditEntry("GitHub CLI", "no local GitHub CLI account", "gh auth status", "not logged in", "yes", "no")]
        return [
            AccountAuditEntry("GitHub CLI", account, "gh local configuration / credential store", "logged in", "yes", "no")
            for account in accounts
        ]

    def _audit_git_credential_manager(self) -> list[AccountAuditEntry]:
        accounts = self.git_credential_manager_accounts()
        if accounts is None:
            return [self._unsupported("Git Credential Manager", "git credential-manager")]
        if not accounts:
            return [AccountAuditEntry("Git Credential Manager", "no listed GitHub account", "git credential-manager github list", "not configured", "yes", "no")]
        return [
            AccountAuditEntry("Git Credential Manager", account, "Git Credential Manager GitHub store", "present", "yes", "no")
            for account in accounts
        ]

    def _audit_vscode(self) -> list[AccountAuditEntry]:
        entries: list[AccountAuditEntry] = []
        if self._app_data is None:
            return [self._unsupported("VS Code", "current-user APPDATA")]
        for name, directory in (("VS Code Stable", self._app_data / "Code"), ("VS Code Insiders", self._app_data / "Code - Insiders")):
            if directory.exists():
                for account_surface in (
                    "GitHub account (not inspected)",
                    "Microsoft account (not inspected)",
                    "GitHub Copilot (not inspected)",
                    "Settings Sync (not inspected)",
                    "Extension authentication state (not inspected)",
                ):
                    entries.append(
                        AccountAuditEntry(
                            name,
                            account_surface,
                            str(directory / "User"),
                            "manual verification required",
                            "no",
                            "yes",
                        )
                    )
        if not entries:
            entries.append(AccountAuditEntry("VS Code", "local user-data profile not found", str(self._app_data), "no local evidence found", "no", "yes"))
        return entries

    def _audit_remote_tunnels(self) -> list[AccountAuditEntry]:
        entries: list[AccountAuditEntry] = []
        for command in self._code_commands:
            state = self.tunnel_status(command)
            entries.append(
                AccountAuditEntry(
                    f"VS Code Remote Tunnel ({command})",
                    "registered tunnel details not inspected",
                    f"{command} tunnel status",
                    state,
                    "yes" if state == "possibly registered" else "no",
                    "yes",
                )
            )
        return entries

    def _audit_credentials(self) -> list[AccountAuditEntry]:
        credentials = self._credential_store.list_supported_entries()
        if credentials is None:
            return [self._unsupported("Windows Credential Manager", "current-user Credential Manager")]
        if not credentials:
            return [AccountAuditEntry("Windows Credential Manager", "no supported developer credential", "current-user Credential Manager", "no local evidence found", "yes", "no")]
        return [
            AccountAuditEntry("Windows Credential Manager", entry.username, entry.target_name, "present", "yes", "no")
            for entry in credentials
        ]

    def _audit_devtunnel(self) -> list[AccountAuditEntry]:
        state = self.devtunnel_login_status()
        return [
            AccountAuditEntry(
                "Microsoft Dev Tunnels",
                "CLI login (account details not displayed)",
                f"{self._devtunnel_command} user show",
                state,
                "no" if state == "unsupported" else "yes",
                "yes" if state in {"unsupported", "unknown"} else "no",
            )
        ]

    def _audit_tailscale_mac_mirror(self) -> list[AccountAuditEntry]:
        entries: list[AccountAuditEntry] = []
        ts_state = self.tailscale_userspace_status()
        entries.append(
            AccountAuditEntry(
                "Tailscale userspace",
                "mac-mirror session / state (account details not displayed)",
                str(self._tailscale_state_dir / "tailscaled.state"),
                ts_state,
                "yes" if ts_state != "not logged in" else "no",
                "no",
            )
        )
        prefs = self.mac_mirror_prefs_path
        if prefs is not None and (prefs.exists() or prefs.is_symlink()):
            entries.append(
                AccountAuditEntry(
                    "Mac Mirror Viewer",
                    "UI preferences (no credentials stored)",
                    str(prefs),
                    "present",
                    "yes",
                    "no",
                )
            )
        return entries

    def _audit_environment(self) -> list[AccountAuditEntry]:
        current_names = self._environment_store.list_token_names()
        audit_names = self._environment_store.audit_names()
        if current_names is None or audit_names is None:
            return [self._unsupported("Current-user environment", "HKCU\\Environment")]
        return [
            AccountAuditEntry(
                "Current-user environment",
                name,
                "HKCU\\Environment",
                "present" if name in current_names else "absent",
                "yes" if name in current_names else "no",
                "no",
            )
            for name in audit_names
        ]

    def _remove_github_cli(self, apply: bool, confirm: Callable[[str, str], bool]) -> list[ActionResult]:
        accounts = self.github_cli_accounts()
        if accounts is None:
            return [self._action("GitHub CLI", "github.com", "gh", "unsupported", "GitHub CLI is not installed")]
        return [
            self._remove_command(
                "GitHub CLI", account, "gh auth status", ("gh", "auth", "logout", "--hostname", "github.com", "--user", account),
                apply, confirm, f"REMOVE GH {account}", "local sign-out only; remote OAuth token is not revoked",
            )
            for account in accounts
        ]

    def _remove_gcm(self, apply: bool, confirm: Callable[[str, str], bool]) -> list[ActionResult]:
        accounts = self.git_credential_manager_accounts()
        if accounts is None:
            return [self._action("Git Credential Manager", "GitHub authentication", "git credential-manager", "unsupported", "command is not installed")]
        return [
            self._remove_command(
                "Git Credential Manager", account, "Git Credential Manager GitHub store",
                ("git", "credential-manager", "github", "logout", account), apply, confirm,
                f"REMOVE GCM {account}", "removed through Git Credential Manager",
            )
            for account in accounts
        ]

    def _remove_tunnels(self, apply: bool, confirm: Callable[[str, str], bool]) -> list[ActionResult]:
        results: list[ActionResult] = []
        for command in self._code_commands:
            state = self.tunnel_status(command)
            if state == "unsupported":
                results.append(self._action("VS Code Remote Tunnel", command, f"{command} tunnel", "unsupported", "VS Code CLI is not installed"))
            elif state == "possibly registered":
                results.append(
                    self._remove_command(
                        "VS Code Remote Tunnel", command, f"{command} tunnel unregister",
                        (command, "tunnel", "unregister"), apply, confirm,
                        f"UNREGISTER TUNNEL {command}", "unregistered through the official VS Code CLI",
                    )
                )
        return results

    def _remove_devtunnel(self, apply: bool, confirm: Callable[[str, str], bool]) -> list[ActionResult]:
        location = f"{self._devtunnel_command} user logout"
        if self.devtunnel_login_status() == "unsupported":
            return [self._action(
                "Microsoft Dev Tunnels", "cached CLI login", location, "unsupported",
                "Dev Tunnel CLI is unavailable; restore devtunnel.exe to remove cached login through the official CLI",
            )]

        # Run logout even if user show reports signed out: expired credentials
        # may still be cached. Never delete a guessed credential-store path.
        try:
            result = self._remove_command(
                "Microsoft Dev Tunnels", "cached CLI login", location,
                (self._devtunnel_command, "user", "logout"), apply, confirm,
                "LOGOUT DEVTUNNEL",
                "cleared cached CLI login; close active SSH/tunnel sessions separately; browser cookies are separate",
            )
        except OSError:
            return [self._action(
                "Microsoft Dev Tunnels", "cached CLI login", location, "error",
                "could not start Dev Tunnel logout; no exception output is shown",
            )]
        if result.status == "removed":
            state = self.devtunnel_login_status()
            if state != "not logged in":
                status = "error" if state == "logged in" else "manual"
                result = self._action(
                    "Microsoft Dev Tunnels", "cached CLI login", location, status,
                    "logout completed but sign-out could not be confirmed; check devtunnel user show manually",
                )
        return [result]

    def _safe_unlink_file(self, path: Path, root: Path, provider: str) -> str | None:
        """Safely unlink a single regular file under root; return error reason or None."""
        if not path.exists() and not path.is_symlink():
            return None
        reason = PathCleaner()._unsafe_reason(CleanTarget(provider, path.name, path, "developer-auth", root))
        if reason:
            return reason
        try:
            path.unlink()
        except FileNotFoundError:
            return None
        except OSError:
            return "could not remove local file"
        return None

    def _read_tracked_pids(self) -> list[int]:
        pids: list[int] = []
        for name in ("mac-mirror-viewer.pid", "tailscaled.pid"):
            path = self._tailscale_state_dir / name
            if not path.is_file() or path.is_symlink():
                continue
            try:
                pid = int(path.read_text(encoding="utf-8").strip())
                if pid > 0:
                    pids.append(pid)
            except (OSError, ValueError):
                continue
        tunnel_json = self._tailscale_state_dir / "mac-vnc-tunnel.json"
        if tunnel_json.is_file() and not tunnel_json.is_symlink():
            try:
                data = json.loads(tunnel_json.read_text(encoding="utf-8"))
                pid = int(data.get("pid", 0) or 0) if isinstance(data, dict) else 0
                if pid > 0:
                    pids.append(pid)
            except (OSError, ValueError, json.JSONDecodeError):
                pass
        return pids

    def _remove_tailscale_mac_mirror(self, apply: bool, confirm: Callable[[str, str], bool]) -> list[ActionResult]:
        results: list[ActionResult] = []
        state_files = [self._tailscale_state_dir / name for name in self.TAILSCALE_STATE_FILES]
        has_any_file = any(p.exists() or p.is_symlink() for p in state_files)
        daemon_state = self._query_tailscale_daemon_state()

        if daemon_state is not None or has_any_file:
            location = str(self._tailscale_state_dir / "tailscaled.state")
            if not apply:
                results.append(self._action(
                    "Tailscale userspace", "mac-mirror session / state", location, "dry-run",
                    "would run tailscale logout, stop tracked mac-mirror/tailscaled PIDs, and remove tailscaled.state",
                ))
            elif not confirm("Tailscale userspace session and state", "LOGOUT TAILSCALE"):
                results.append(self._action(
                    "Tailscale userspace", "mac-mirror session / state", location, "manual",
                    "strong confirmation declined",
                ))
            else:
                errors: list[str] = []
                if daemon_state is not None:
                    try:
                        outcome = self._runner.run((self._tailscale_command, f"--socket={self.TAILSCALE_PIPE_SOCKET}", "logout"))
                        if outcome is None or outcome.returncode != 0:
                            errors.append("tailscale logout did not confirm success")
                    except OSError:
                        errors.append("could not start tailscale logout")

                if os.name == "nt":
                    for pid in self._read_tracked_pids():
                        try:
                            self._runner.run(("taskkill", "/PID", str(pid), "/F"))
                        except OSError:
                            pass

                for path in state_files:
                    err = self._safe_unlink_file(path, self._tailscale_state_dir, "Tailscale userspace")
                    if err:
                        errors.append(f"{path.name}: {err}")

                state_file = self._tailscale_state_dir / "tailscaled.state"
                if errors or state_file.exists() or state_file.is_symlink():
                    detail = "; ".join(errors) if errors else "tailscaled.state still exists after cleanup"
                    results.append(self._action("Tailscale userspace", "mac-mirror session / state", location, "error", detail))
                else:
                    results.append(self._action(
                        "Tailscale userspace", "mac-mirror session / state", location, "removed",
                        "logged out Tailscale userspace session and removed local state files",
                    ))

        prefs = self.mac_mirror_prefs_path
        if prefs is not None and (prefs.exists() or prefs.is_symlink()):
            location = str(prefs)
            if not apply:
                results.append(self._action(
                    "Mac Mirror Viewer", "UI preferences", location, "dry-run",
                    "would remove local mac-mirror UI preferences",
                ))
            elif not confirm(f"Mac Mirror Viewer UI preferences '{prefs}'", "REMOVE MAC-MIRROR PREFS"):
                results.append(self._action(
                    "Mac Mirror Viewer", "UI preferences", location, "manual",
                    "strong confirmation declined",
                ))
            else:
                err = self._safe_unlink_file(prefs, prefs.parent, "Mac Mirror Viewer")
                if err:
                    results.append(self._action("Mac Mirror Viewer", "UI preferences", location, "error", err))
                else:
                    results.append(self._action("Mac Mirror Viewer", "UI preferences", location, "removed", "removed local UI preferences"))

        return results

    def _remove_credentials(self, apply: bool, confirm: Callable[[str, str], bool]) -> list[ActionResult]:
        entries = self._credential_store.list_supported_entries()
        if entries is None:
            return [self._action("Windows Credential Manager", "supported developer credentials", "current-user Credential Manager", "unsupported", "Windows Credential API is unavailable")]
        results: list[ActionResult] = []
        for entry in entries:
            phrase = f"DELETE CREDENTIAL {entry.target_name}"
            if not apply:
                results.append(self._action("Windows Credential Manager", entry.username, entry.target_name, "dry-run", "would remove allowlisted current-user credential"))
            elif not confirm(f"Credential Manager entry '{entry.target_name}' for '{entry.username}'", phrase):
                results.append(self._action("Windows Credential Manager", entry.username, entry.target_name, "manual", "strong confirmation declined"))
            elif self._credential_store.delete(entry):
                results.append(self._action("Windows Credential Manager", entry.username, entry.target_name, "removed", "deleted allowlisted current-user credential"))
            else:
                results.append(self._action("Windows Credential Manager", entry.username, entry.target_name, "error", "Credential Manager did not delete the entry"))
        return results

    def _remove_environment(self, apply: bool, confirm: Callable[[str, str], bool]) -> list[ActionResult]:
        names = self._environment_store.list_token_names()
        if names is None:
            return [self._action("Current-user environment", "allowlisted tokens", "HKCU\\Environment", "unsupported", "current-user registry environment is unavailable")]
        results: list[ActionResult] = []
        for name in sorted(names, key=str.casefold):
            if not apply:
                results.append(self._action("Current-user environment", name, "HKCU\\Environment", "dry-run", "would remove current-user variable; value not read"))
            elif not confirm(f"Current-user environment variable '{name}'", f"REMOVE ENV {name}"):
                results.append(self._action("Current-user environment", name, "HKCU\\Environment", "manual", "strong confirmation declined"))
            elif self._environment_store.delete(name):
                results.append(self._action("Current-user environment", name, "HKCU\\Environment", "removed", "removed current-user variable; value was not read"))
            else:
                results.append(self._action("Current-user environment", name, "HKCU\\Environment", "error", "could not remove current-user variable"))
        return results

    def _remove_command(
        self,
        provider: str,
        account: str,
        location: str,
        command: tuple[str, ...],
        apply: bool,
        confirm: Callable[[str, str], bool],
        phrase: str,
        success_detail: str,
    ) -> ActionResult:
        if not apply:
            return self._action(provider, account, location, "dry-run", f"would run supported local logout ({success_detail})")
        if not confirm(f"{provider} account or registration '{account}'", phrase):
            return self._action(provider, account, location, "manual", "strong confirmation declined")
        outcome = self._runner.run(command)
        if outcome is None:
            return self._action(provider, account, location, "unsupported", "required command is not installed")
        if outcome.returncode == 0:
            return self._action(provider, account, location, "removed", success_detail)
        return self._action(provider, account, location, "error", "official logout command reported a failure; no command output is shown")

    @staticmethod
    def _parse_gh_accounts(output: str) -> list[str]:
        accounts = re.findall(r"\baccount\s+([^\s(]+)", output, flags=re.IGNORECASE)
        return list(dict.fromkeys(account for account in accounts if account))

    @staticmethod
    def _parse_gcm_accounts(output: str) -> list[str]:
        accounts: list[str] = []
        for line in output.splitlines():
            stripped = line.strip()
            match = re.search(r"(?:username|user|account)\s*[:=]\s*([^\s]+)", stripped, re.IGNORECASE)
            if match:
                accounts.append(match.group(1))
            elif re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,255}", stripped):
                accounts.append(stripped)
        return list(dict.fromkeys(accounts))

    @staticmethod
    def _unsupported(provider: str, location: str) -> AccountAuditEntry:
        return AccountAuditEntry(provider, "not available", location, "unsupported", "no", "yes")

    @staticmethod
    def _action(provider: str, subject: str, location: str, status: str, details: str) -> ActionResult:
        return ActionResult(provider, subject, location, "developer-auth", status, details)
