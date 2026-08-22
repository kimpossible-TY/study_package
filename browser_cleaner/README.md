# Windows Local Authentication Cleaner

`browser_cleaner` safely audits and removes selected **local, current-user** browser privacy data and developer authentication evidence on Windows. It does not delete a cloud account, revoke remote sessions/tokens, or access another Windows user account.

## Two-command everyday use

Most users need only these two commands:

```powershell
# 1. Review every recognized browser target plus supported developer-authentication candidates.
python .\main.py

# 2. Apply that same cleanup.
python .\main.py --apply
```

The first command is always dry run. The second first shows the exact browser paths, requires `CLEAN` for ordinary browser data, and then requires a separate strong confirmation for every account, credential, tunnel, environment token, or full profile. `--yes` cannot bypass sensitive confirmations.

Run it on Windows. A non-Windows host reports discovery only and refuses Windows-oriented destructive work.

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

With no selector, the two-command mode selects every normal browser category **and** supported developer-authentication candidates. Use categories only when you want a narrower advanced operation.

```powershell
# Preview Chrome cookies/site data only; no history/password selection.
python .\main.py --browser chrome --category site-data

# Delete Firefox session files after the standard confirmation.
python .\main.py --browser firefox --category sessions --apply

# Delete history and cache across supported browsers.
python .\main.py --category history --category cache --apply

# --yes skips only the ordinary browser CLEAN prompt.
python .\main.py --category cache --apply --yes
```

Close browsers normally first. `--kill` is an explicit, forceful compatibility option and can lose unsaved work. Cookie/site-data removal usually signs websites out, but does not reliably sign out Chrome, Edge, Firefox, or Brave sync accounts. Turn sync off and sign out in each browser profile/account menu first; deleting while sync is active can affect other devices.

## Account audit

`python .\main.py` already provides this audit as part of the simple review flow. `--audit-accounts` remains available when you only want the account table.

The audit shows only program/provider, account name or username, location or Credential Manager target, login state, automatic-removal support, and whether manual action is needed. It never displays token, password, cookie, or credential-secret values.

Best-effort audit sources are GitHub CLI (`gh auth status`), Git Credential Manager (`git credential-manager github list`), Chrome/Edge/Firefox/Brave profile presence (sync account state is deliberately not inspected), VS Code Stable/Insiders profile presence without reading auth databases, VS Code CLI tunnel status, supported Windows Credential Manager target metadata, and current-user environment-variable **names**. The allowlist is `GH_TOKEN`, `GITHUB_TOKEN`, `GITHUB_ENTERPRISE_TOKEN`, `AZURE_DEVOPS_EXT_PAT`, and `AZURE_*`. Missing commands are `unsupported`, never fatal.

Git `user.name` and `user.email` are commit identity settings, not authentication; the tool never removes them.

## Developer authentication

The two-command flow already includes this. `--category developer-auth` remains available as an advanced developer-only preview or cleanup.

`--yes` never bypasses sensitive confirmation. GitHub CLI uses:

```powershell
gh auth logout --hostname github.com --user <username>
```

That removes local CLI authentication only; it does **not** revoke the remote OAuth token. Git Credential Manager uses `git credential-manager github logout <username>` when installed. VS Code Remote Tunnel uses `code tunnel unregister` or `code-insiders tunnel unregister` when status indicates a possible registration.

Credential Manager is accessed through the Windows Credential API, not localized `cmdkey /list` parsing. Only targets clearly related to GitHub, Git Credential Manager, VS Code, or Azure are candidates. RDP, SMB, Wi-Fi, corporate-network, and unknown credentials are not selected. Environment-token removal only operates in `HKCU\Environment`; it never changes Machine-scope variables.

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

Verification checks `gh auth status`, Git Credential Manager listing, allowlisted Credential Manager targets, tunnel status, current-user token-variable names, and selected browser artifacts. Results are conservative: `verified removed`, `no local evidence found`, `manual verification required`, or `remote revocation recommended`. VS Code account/sync state still needs the UI steps above.

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
