# study_package

Subprojects:

- [windows_local_auth_cleaner](windows_local_auth_cleaner/README.md)

`computer_setting.ps1` bootstraps the current user's Windows development tools,
including Microsoft's standalone Dev Tunnel CLI. It installs `devtunnel.exe` under
`%LOCALAPPDATA%\Programs\devtunnel` and configures the user/session `PATH` without
automatically signing in. On a shared PC, use `devtunnel user login -g -d` and approve
the device code on your own phone or trusted computer. Before leaving, exit SSH and
stop active tunnels, then run `wac` to clean supported browser data and developer
authentication, including the cached Dev Tunnel login. See the cleaner's
[Dev Tunnel instructions](windows_local_auth_cleaner/README.md#microsoft-dev-tunnels-on-a-shared-pc).

The bootstrapper also installs PSReadLine (2.1 or newer) and saves Tab menu
completion, history predictions, Up/Down history search, oh-my-posh initialization,
and the global `mac` SSH function in each edition's current-user all-hosts profile
(`Documents\WindowsPowerShell\profile.ps1` and `Documents\PowerShell\profile.ps1`).
Redirected Documents folders are supported. Run `.\computer_setting.ps1`, then
open a new PowerShell window. `mac` connects to `taeyoung@127.0.0.1` after the Dev
Tunnel connection is ready; additional arguments are forwarded to `ssh`.

`wac` removes the managed settings during default/developer-auth cleanup. Existing
unrelated profile content is preserved; profiles containing only the managed
settings are deleted. Close existing shells after cleanup to unload `mac` and the
prompt. `computer_setting.ps1` installs the pinned v0.2.0 release through its standalone
installer, verifies the executable SHA-256, and refreshes the copy in Downloads.
Re-running setup replaces older installed executables, including when old Scoop
shims exist.
