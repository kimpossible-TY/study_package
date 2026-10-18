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
