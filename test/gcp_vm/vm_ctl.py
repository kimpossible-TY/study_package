#!/usr/bin/env python3
"""Helper CLI for interacting with the temporary GCP Windows Server 2022 test VM."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SCRATCH_DIR = Path(
    "/Users/taeyoung/.gemini/antigravity/brain/847eb5cc-70f9-487c-bf21-f01dceed58e6/scratch/gcp_vm"
)
CONFIG_PATH = SCRATCH_DIR / "vm_config.json"


def load_config() -> dict:
    if CONFIG_PATH.exists():
        return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    return {
        "project": "gen-lang-client-0651335642",
        "zone": "asia-northeast3-a",
        "instance": "mac-mirror-win-test",
        "host": "34.47.111.231",
        "user": "tester",
        "ssh_key": str(SCRATCH_DIR / "id_ed25519"),
    }


def ssh_base_args(cfg: dict) -> list[str]:
    return [
        "-i",
        cfg["ssh_key"],
        "-o",
        "BatchMode=yes",
        "-o",
        "StrictHostKeyChecking=no",
        "-o",
        "UserKnownHostsFile=/dev/null",
        "-o",
        "LogLevel=ERROR",
        "-o",
        "ConnectTimeout=10",
    ]


def run_ssh(cfg: dict, remote_cmd: str, check: bool = True) -> subprocess.CompletedProcess:
    cmd = ["ssh", *ssh_base_args(cfg), f"{cfg['user']}@{cfg['host']}", remote_cmd]
    return subprocess.run(cmd, check=check)


def upload_file(cfg: dict, local_path: str, remote_path: str) -> None:
    cmd = [
        "scp",
        "-O",
        "-q",
        *ssh_base_args(cfg),
        local_path,
        f"{cfg['user']}@{cfg['host']}:{remote_path}",
    ]
    subprocess.run(cmd, check=True)


def download_file(cfg: dict, remote_path: str, local_path: str) -> None:
    Path(local_path).parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        "scp",
        "-O",
        "-q",
        *ssh_base_args(cfg),
        f"{cfg['user']}@{cfg['host']}:{remote_path}",
        local_path,
    ]
    subprocess.run(cmd, check=True)


def desktop_run(
    cfg: dict,
    ps_body: str,
    background: bool = False,
    cwd: str = r"C:\mac-mirror-test",
    timeout: int = 30,
) -> int:
    with tempfile.NamedTemporaryFile("w", suffix=".ps1", delete=False, encoding="utf-8") as tmp:
        tmp.write(ps_body + "\n")
        tmp_path = Path(tmp.name)
    try:
        remote_payload = r"C:/mac-mirror-test/_payload.ps1"
        upload_file(cfg, str(tmp_path), remote_payload)
        bg_flag = " -Background" if background else ""
        remote_cmd = (
            "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass "
            f"-File C:\\mac-mirror-test\\desktop_exec.ps1 "
            f"-CommandLine \"& 'C:\\mac-mirror-test\\_payload.ps1'\" "
            f"-WorkingDirectory \"{cwd}\" -TimeoutSeconds {timeout}{bg_flag}"
        )
        proc = run_ssh(cfg, remote_cmd, check=False)
        return proc.returncode
    finally:
        tmp_path.unlink(missing_ok=True)


def ensure_tunnel(cfg: dict) -> None:
    probe = run_ssh(
        cfg,
        r"powershell -NoProfile -ExecutionPolicy Bypass -File C:\mac-mirror-test\verify_socks5.ps1",
        check=False,
    )
    if probe.returncode == 0:
        return

    subprocess.run(["pkill", "-f", "15900:127.0.0.1:5900"], check=False)
    tunnel_cmd = [
        "ssh",
        "-f",
        "-N",
        *ssh_base_args(cfg),
        "-o",
        "ExitOnForwardFailure=yes",
        "-o",
        "ServerAliveInterval=15",
        "-o",
        "ServerAliveCountMax=3",
        "-R",
        "127.0.0.1:15900:127.0.0.1:5900",
        "-L",
        "127.0.0.1:13389:127.0.0.1:3389",
        f"{cfg['user']}@{cfg['host']}",
    ]
    subprocess.run(tunnel_cmd, check=True)
    run_ssh(
        cfg,
        r"powershell -NoProfile -ExecutionPolicy Bypass -File C:\mac-mirror-test\verify_socks5.ps1",
        check=True,
    )


def ensure_desktop(cfg: dict) -> None:
    res = subprocess.run(
        ["ssh", *ssh_base_args(cfg), f"{cfg['user']}@{cfg['host']}", "query session"],
        capture_output=True,
        text=True,
        check=False,
    )
    if "Active" in (res.stdout or ""):
        print("Interactive desktop session is Active.")
        return

    ensure_tunnel(cfg)
    creds_path = SCRATCH_DIR / "rdp_creds.json"
    creds = json.loads(creds_path.read_text(encoding="utf-8"))
    subprocess.run(["pkill", "-f", "sdl-freerdp"], check=False)
    env = os.environ.copy()
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    log_file = open(SCRATCH_DIR / "freerdp.log", "w", encoding="utf-8")
    subprocess.Popen(
        [
            "/opt/homebrew/bin/sdl-freerdp",
            "/v:127.0.0.1:13389",
            f"/u:{creds['username']}",
            f"/p:{creds['password']}",
            "/size:1920x1080",
            "/bpp:32",
            "/cert:ignore",
            "-clipboard",
            "/audio-mode:2",
        ],
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    time.sleep(8)
    print("Started headless 1920x1080 interactive desktop session.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="subcmd", required=True)

    p_ssh = sub.add_parser("ssh", help="Run command over SSH on Windows VM")
    p_ssh.add_argument("command")

    p_up = sub.add_parser("upload", help="Upload local file to Windows VM")
    p_up.add_argument("local_path")
    p_up.add_argument("remote_path")

    p_down = sub.add_parser("download", help="Download remote file from Windows VM")
    p_down.add_argument("remote_path")
    p_down.add_argument("local_path")

    p_desk = sub.add_parser("desktop-run", help="Run PowerShell command inside active Console desktop")
    p_desk.add_argument("command")
    p_desk.add_argument("--bg", action="store_true", help="Launch detached in interactive desktop")
    p_desk.add_argument("--cwd", default=r"C:\mac-mirror-test")
    p_desk.add_argument("--timeout", type=int, default=30)

    p_shot = sub.add_parser("screenshot", help="Capture full-screen PNG from active Windows desktop")
    p_shot.add_argument("local_png")

    p_in = sub.add_parser("input", help="Send mouse/keyboard input to active Windows desktop")
    p_in.add_argument("action", choices=["move", "click", "key", "hotkey", "text"])
    p_in.add_argument("arg1", nargs="?", default="")
    p_in.add_argument("arg2", nargs="?", default="")

    sub.add_parser("ensure-tunnel", help="Ensure SSH reverse tunnel + SOCKS5 127.0.0.1:1055 -> Mac 5900")
    sub.add_parser("ensure-desktop", help="Ensure active 1920x1080 interactive desktop session")

    args = parser.parse_args()
    cfg = load_config()

    if args.subcmd == "ssh":
        return run_ssh(cfg, args.command, check=False).returncode
    if args.subcmd == "upload":
        upload_file(cfg, args.local_path, args.remote_path)
        return 0
    if args.subcmd == "download":
        download_file(cfg, args.remote_path, args.local_path)
        return 0
    if args.subcmd == "desktop-run":
        return desktop_run(cfg, args.command, background=args.bg, cwd=args.cwd, timeout=args.timeout)
    if args.subcmd == "screenshot":
        remote_png = r"C:\mac-mirror-test\latest_screen.png"
        rc = desktop_run(
            cfg,
            f"& 'C:\\mac-mirror-test\\capture_screenshot.ps1' -OutPath '{remote_png}'",
        )
        if rc != 0:
            return rc
        download_file(cfg, "C:/mac-mirror-test/latest_screen.png", args.local_png)
        print(f"Saved screenshot -> {args.local_png}")
        return 0
    if args.subcmd == "input":
        a1 = args.arg1.replace("'", "''")
        a2 = args.arg2.replace("'", "''")
        ps = f"& 'C:\\mac-mirror-test\\ui_input.ps1' -Action '{args.action}' -Arg1 '{a1}' -Arg2 '{a2}'"
        return desktop_run(cfg, ps)
    if args.subcmd == "ensure-tunnel":
        ensure_tunnel(cfg)
        return 0
    if args.subcmd == "ensure-desktop":
        ensure_desktop(cfg)
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
