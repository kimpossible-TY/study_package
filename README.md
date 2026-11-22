# study_package

Subprojects:

- [windows_local_auth_cleaner](windows_local_auth_cleaner/README.md)

## Quick Start (Windows)

Run the one-shot bootstrapper directly in Windows PowerShell (no administrator privileges required):

```powershell
irm https://raw.githubusercontent.com/kimpossible-TY/study_package/main/computer_setting.ps1 | iex
```

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
the global `mac` SSH function, and the one-shot `mac-mirror` / `mac-mirror-stop`
screen mirroring shortcuts in each edition's current-user all-hosts profile
(`Documents\WindowsPowerShell\profile.ps1` and `Documents\PowerShell\profile.ps1`).
Redirected Documents folders are supported. Run `.\computer_setting.ps1`, then
open a new PowerShell window. `mac` connects to `taeyoung@127.0.0.1` after the Dev
Tunnel connection is ready; additional arguments are forwarded to `ssh`.

## One-shot Windows → Mac Screen Mirroring (No Admin Required)

`computer_setting.ps1` installs and configures everything needed to mirror a Mac over
Tailscale from a standard (non-admin) Windows account:

- **Tailscale userspace binaries** (`tailscale.exe`, `tailscaled.exe`) extracted via
  `msiexec /a` without admin privileges into `%LOCALAPPDATA%\Programs\tailscale`
- **Native Direct3D 11 Mac Mirror Viewer** (`mac-mirror-viewer.exe`, built from
  [`mac_mirror_viewer/`](mac_mirror_viewer)) installed into
  `%LOCALAPPDATA%\Programs\mac-mirror\mac-mirror-viewer.exe` (zero external DLLs;
  automatic fallback to **TigerVNC Viewer 1.16.2** in
  `%LOCALAPPDATA%\Programs\tigervnc\vncviewer.exe` if the viewer binary is not yet
  available)
- **SOCKS5 VNC launcher & fallback tunnel** ([`mac_vnc_tunnel.py`](mac_vnc_tunnel.py))
  installed into `%LOCALAPPDATA%\Programs\mac-mirror` along with `mac-mirror` /
  `mac-mirror-stop` shortcuts

Running `.\computer_setting.ps1` sets up the environment and automatically invokes
`mac-mirror` at the end (pass `-SkipMacMirror` to install tools without connecting
immediately). `mac-mirror` performs the full one-shot sequence:

1. Starts `tailscaled.exe` in userspace-networking mode (`--windows-mode=dev`,
   `--tun=userspace-networking`, `--socket=\\.\pipe\tailscale-dev`,
   `--socks5-server=127.0.0.1:1055`).
2. Runs `tailscale up --unattended` (opening browser sign-in if needed).
3. Auto-resolves the shared Mac's Tailscale IP (`kimtaeyoungs-macbook-air*`, or
   override with `mac-mirror --host <IP>`).
4. Launches `mac-mirror-viewer.exe --host <MacIP> --port 5900 --socks5 127.0.0.1:1055`
   directly over Tailscale SOCKS5 with zero extra local TCP relay hop:
   - Opens immediately in borderless fullscreen (`16:10` aspect-preserved Direct3D 11 /
     Direct2D Retina scaling) with a dark one-click Login Modal supporting macOS
     Screen Sharing Apple Diffie-Hellman (`SecurityType 30`) and `VncAuth` (`2`), plus
     a `⚙` toggle for Target Host / SOCKS5 Proxy settings.
   - Renders a zero-latency anti-aliased vector macOS cursor (`1.0x`–`4.0x`, default
     `1.8x`; `Ctrl+Alt+=` / `Ctrl+Alt+-`) and an auto-hide floating pill HUD at the top
     edge or via `Ctrl+Alt+Space` (`FPS · ms`, cursor scale slider, `Adaptive` /
     `Sharp` / `Turbo` quality modes, `Ctrl+Alt+F` fullscreen toggle).
   - **Shared-PC security:** Username and password are kept strictly in memory and
     wiped after authentication — never saved to disk (`ui-prefs.json` stores only
     non-sensitive `cursor_scale`, `quality_mode`, and `fullscreen`).
   - If `mac-mirror-viewer.exe` is absent (or `--force-tigervnc` is passed),
     `mac_vnc_tunnel.py` automatically starts the local SOCKS5 TCP forwarder
     (`127.0.0.1:15900`) and launches `vncviewer.exe -DotWhenNoCursor=1 127.0.0.1:15900`.

Before leaving a shared PC, run `wac` (which now also logs out Tailscale userspace,
removes `%USERPROFILE%\.tailscale\tailscaled.state` and `ui-prefs.json`, and cleans
the managed PowerShell profile block including `mac` / `mac-mirror` / `mac-mirror-stop`),
or run `mac-mirror-stop --logout` directly.

`wac` removes the managed settings during default/developer-auth cleanup. Existing
unrelated profile content is preserved; profiles containing only the managed
settings are deleted. Close existing shells after cleanup to unload `mac`, `mac-mirror`,
and the prompt. `computer_setting.ps1` installs the pinned v0.3.0 release through its
standalone installer, verifies the executable SHA-256, and refreshes the copy in
Downloads. Re-running setup replaces older installed executables, including when old
Scoop shims exist.
