# Windows Local Authentication Cleaner

`windows_local_auth_cleaner` safely audits and removes selected **local, current-user** browser privacy data and developer authentication evidence on Windows. It does not delete a cloud account, revoke remote sessions/tokens, or access another Windows user account.

## Install once and run anywhere

The repository does not need to stay on the Windows machine after installation.

### Option A: one-line install from GitHub Release (Recommended)

Run this one-liner in PowerShell (no Python, Git clone, or repository checkout required):

```powershell
irm https://raw.githubusercontent.com/kimpossible-TY/study_package/main/windows_local_auth_cleaner/install.ps1 | iex
```

This installer:
- Downloads the latest standalone `windows-local-auth-cleaner.exe` directly from the GitHub Release.
- Installs it into `$env:LOCALAPPDATA\Programs\windows-local-auth-cleaner`.
- Creates the `wac` command alias.
- Configures your user `PATH` (and Scoop shims if present) so `windows-local-auth-cleaner` and `wac` can be invoked immediately from any terminal.

To install a specific version or uninstall:
```powershell
# Specific tag:
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/kimpossible-TY/study_package/main/windows_local_auth_cleaner/install.ps1))) -Tag "v0.1.0"

# Uninstall:
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/kimpossible-TY/study_package/main/windows_local_auth_cleaner/install.ps1))) -Uninstall
```

### Option B: install with Scoop

If you use [Scoop](https://scoop.sh), install directly using the Scoop manifest:

```powershell
scoop install https://raw.githubusercontent.com/kimpossible-TY/study_package/main/windows_local_auth_cleaner/windows-local-auth-cleaner.json
```

Scoop downloads the binary from GitHub Releases and shims both `windows-local-auth-cleaner` and `wac` into your PATH.

### Option C: download standalone `.exe` directly

Download `windows-local-auth-cleaner.exe` from the latest [GitHub Release](https://github.com/kimpossible-TY/study_package/releases) and run it:

```powershell
.\windows-local-auth-cleaner.exe
```

The GitHub Actions workflow builds and attaches `windows-local-auth-cleaner.exe` automatically whenever a release is created, a tag (`v*` or `windows-local-auth-cleaner-v*`) is pushed, or the workflow is dispatched.

### Option D: install via pipx (Python environment)

If Python is already available:

```powershell
scoop install pipx
pipx ensurepath
pipx install .
```

## Everyday use: one-shot cleanup by default

By default, the tool executes unified cleanup in **one shot** without requiring interactive prompts or manual browser closing:

```powershell
# 1. Run one-shot cleanup immediately (closes running browsers automatically, applies cleanup without prompts)
python .\main.py
# Or if installed via pipx:
wac

# 2. Preview targets without deleting anything (dry-run mode):
python .\main.py --dry-run

# 3. Open the interactive Textual TUI:
python .\main.py --interactive
```

In default one-shot mode:
- Running browsers are **automatically closed** (first attempting a graceful shutdown via `taskkill /im` so SQLite handles and cache flushes cleanly, falling back to force-close if still active). No manual intervention or `--kill` flag is needed.
- Browser privacy data and developer authentication records are cleaned directly without stopping for manual input.
- To preview changes safely without modifying anything, pass `--dry-run`.
- To require manual confirmation prompts, pass `--prompt`.
- To prevent automatically closing running browsers, pass `--keep-browsers`.

## Browser categories

Use `--category` once or repeatedly to select only what is needed:

| Category | Local data |
| --- | --- |
| `sessions` | session restore and session-storage files |
| `history` | browsing/search history artifacts |
| `passwords` | saved-login databases/files |
| `autofill` | form/autofill databases |
| `site-data` | cookies, Local Storage, IndexedDB, service-worker/site storage |
| `cache` | browser caches |
| `developer-auth` | supported developer authentication only |
| `full-profile` | only with an exact profile path or VS Code profile option |

With no selector, one-shot cleanup selects every normal browser category **and** supported developer-authentication candidates. Use categories only when you want a narrower operation.

```powershell
# One-shot cleanup for Chrome cookies/site data only:
python .\main.py --browser chrome --category site-data

# Preview Chrome cookies/site data only (dry-run):
python .\main.py --browser chrome --category site-data --dry-run

# Delete Firefox session files:
python .\main.py --browser firefox --category sessions

# Require explicit confirmation before deleting:
python .\main.py --prompt
```

Running browsers are automatically closed during cleanup to ensure database files are not locked. Cookie/site-data removal usually signs websites out, but does not reliably sign out Chrome, Edge, Firefox, or Brave sync accounts. Turn sync off and sign out in each browser profile/account menu first; deleting while sync is active can affect other devices.

## Account audit

`python .\main.py` shows a compact account check in the Textual results view. Use `python .\main.py --audit-accounts --no-tui` when you only want the full account table.

The audit shows only program/provider, account name or username, location or Credential Manager target, login state, automatic-removal support, and whether manual action is needed. It never displays token, password, cookie, or credential-secret values.

Best-effort audit sources are GitHub CLI (`gh auth status`), Git Credential Manager (`git credential-manager github list`), Microsoft Dev Tunnels (`devtunnel user show`), Chrome/Edge/Firefox/Brave profile presence (sync account state is deliberately not inspected), VS Code Stable/Insiders profile presence without reading auth databases, VS Code CLI tunnel status, supported Windows Credential Manager target metadata, and current-user environment-variable **names**. The allowlist is `GH_TOKEN`, `GITHUB_TOKEN`, `GITHUB_ENTERPRISE_TOKEN`, `AZURE_DEVOPS_EXT_PAT`, and `AZURE_*`. Missing commands are `unsupported`, never fatal.

Git `user.name` and `user.email` are commit identity settings, not authentication; the tool never removes them.

## Developer authentication

The two-command flow already includes this. `--category developer-auth` remains available as an advanced developer-only preview or cleanup.

`--yes` never bypasses sensitive confirmation. GitHub CLI uses:

```powershell
gh auth logout --hostname github.com --user <username>
```

That removes local CLI authentication only; it does **not** revoke the remote OAuth token. Git Credential Manager uses `git credential-manager github logout <username>` when installed. VS Code Remote Tunnel uses `code tunnel unregister` or `code-insiders tunnel unregister` when status indicates a possible registration.

Credential Manager is accessed through the Windows Credential API, not localized `cmdkey /list` parsing. Only targets clearly related to GitHub, Git Credential Manager, VS Code, or Azure are candidates. RDP, SMB, Wi-Fi, corporate-network, and unknown credentials are not selected. Environment-token removal only operates in `HKCU\Environment`; it never changes Machine-scope variables.

### Microsoft Dev Tunnels on a shared PC

[`computer_setting.ps1`](../computer_setting.ps1) installs the official standalone `devtunnel.exe` into `$env:LOCALAPPDATA\Programs\devtunnel` and adds that directory to the current-user and current-session `PATH`. An already available CLI is reused. Installation does not sign in, create a tunnel, or generate an SSH key.

Sign in using a device code so GitHub login can happen on your own phone or trusted computer:

```powershell
devtunnel user login -g -d
devtunnel connect YOUR_TUNNEL_ID
```

The CLI still caches a login token on the shared PC. Deleting `devtunnel.exe` alone does not clear that token. Before cleanup, exit all SSH sessions and stop running `devtunnel connect`/`host` commands with Ctrl+C, then run:

```powershell
# Preview supported developer-auth cleanup:
wac --category developer-auth --dry-run

# Clean supported browser data and developer authentication, including Dev Tunnels:
wac

# Check developer-auth state without modifying it:
wac --category developer-auth --verify
```

Dev Tunnel cleanup runs the official `devtunnel user logout`, then checks `devtunnel user show`. A failed logout, remaining login, or unrecognized status is reported as an error or manual check, never as confirmed removal. Audit output does not display the raw CLI output or tokens. The cleaner also looks in the bootstrapper's install directory if the executable is missing from `PATH`.

This clears the CLI's cached login through its supported interface. It does not terminate existing SSH/tunnel processes, delete SSH keys or shell history, or revoke cloud tokens. Browser cookies are a separate cleanup category; a developer-auth-only cleanup does not remove them. If the CLI is missing, restore `devtunnel.exe` before cleanup rather than guessing which credential-store files to delete. See Microsoft's [authentication and logout reference](https://learn.microsoft.com/en-us/azure/developer/dev-tunnels/cli-commands) and [official executable download](https://learn.microsoft.com/en-us/azure/developer/dev-tunnels/get-started).

## VS Code

VS Code has no stable non-interactive logout CLI that covers GitHub, Microsoft, Copilot, Settings Sync, extension authentication, and every UI tunnel state. This tool never edits VS Code SQLite or authentication state directly.

For Stable and Insiders, use **Accounts** to sign out of GitHub/Microsoft/Copilot/extension accounts; then use **Manage > Settings Sync > Turn Off** for this device. For UI-managed tunnels, run **Remote Tunnels: Turn off Remote Tunnel Access...** from the Command Palette.

A safe backup exports only `settings.json`, `keybindings.json`, snippets, and installed extension names; it excludes `globalStorage`, `workspaceStorage`, cookies, authentication databases, and session data:

```powershell
python .\main.py --backup-vscode-profile D:\handoff-backup\vscode-safe
```

The separate full local profile option needs `--apply` plus an unskippable path-specific confirmation:

```powershell
python .\main.py --wipe-vscode-profile
python .\main.py --wipe-vscode-profile --apply
```

It can remove VS Code settings, keybindings, snippets, workspace state, extension state, and local chat/session data. This local data is not recoverable through the tool.

## Browser full profiles

Never partially edit browser private state files to force sync-account logout. If necessary, request one exact discovered profile under a supported current-user root:

```powershell
python .\main.py --wipe-browser-profile "$env:LOCALAPPDATA\Google\Chrome\User Data\Default"
python .\main.py --wipe-browser-profile "$env:LOCALAPPDATA\Google\Chrome\User Data\Default" --apply
```

The path must exactly match discovery; paths outside known roots, path traversal, symlinks, and Windows reparse/junction points are refused. `--yes` cannot skip the full-profile confirmation.

## Verification and remote revocation

```powershell
python .\main.py --verify
python .\main.py --category developer-auth --verify
```

Verification checks `gh auth status`, Git Credential Manager listing, `devtunnel user show`, allowlisted Credential Manager targets, VS Code tunnel status, current-user token-variable names, and selected browser artifacts. Results are conservative: `verified removed`, `no local evidence found`, `manual verification required`, or `remote revocation recommended`. VS Code account/sync state still needs the UI steps above.

For a lost or handed-off computer, separately review GitHub OAuth Applications, personal access tokens, SSH keys, and active sessions; Microsoft account devices/sessions; and Google account devices/sessions. This tool never revokes them remotely.

## Safety and device handoff

- Without `--apply`, no local data changes.
- Full profiles, credentials, and token variables require separate strong confirmations that `--yes` cannot bypass.
- Targets must be inside a known current-user root. System directories, an entire home directory, other users, and all Credential Manager entries are never selected.
- Partial failures are independently reported as `removed`, `dry-run`, `manual`, `unsupported`, `locked`, `error`, or `missing`.

The most certain local-device cleanup is removing the old Windows user profile, which this tool never does: back up files; create a separate local administrator; sign out; sign in to that administrator; then use **Settings > Accounts > Other users** to remove the prior user and data.

## Structure

- `models.py`: options, targets, audit/action/verification models
- `config.py`, `profiles.py`, `cleaning.py`: category discovery and safe path deletion
- `developer_auth.py`: supported CLI/API audit and logout/removal
- `vscode.py`: safe backup and explicit profile wipe
- `verification.py`: non-destructive local verification
