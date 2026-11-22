#!/usr/bin/env python3
"""Userspace Tailscale SOCKS5 VNC tunnel and one-shot Mac screen mirroring launcher.

Designed for standard (non-admin) Windows accounts where Tailscale runs in
userspace-networking mode (SOCKS5 proxy on 127.0.0.1:1055). Prefers launching the
native Direct3D 11 viewer (`mac-mirror-viewer.exe --host <MacIP> --port 5900
--socks5 127.0.0.1:1055`) directly over Tailscale SOCKS5 with zero extra relay hop,
while retaining `--tunnel-only` and automatic TigerVNC Viewer (`127.0.0.1:15900`)
fallback support.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

DEFAULT_MAC_HOST = "100.80.74.94"
DEFAULT_PEER_MATCH = "kimtaeyoungs-macbook-air"
DEFAULT_REMOTE_PORT = 5900
DEFAULT_LISTEN_HOST = "127.0.0.1"
DEFAULT_LISTEN_PORT = 15900
DEFAULT_SOCKS_HOST = "127.0.0.1"
DEFAULT_SOCKS_PORT = 1055
DEFAULT_PIPE_SOCKET = r"\\.\pipe\tailscale-dev"

SOCKS5_ERRORS = {
    1: "general SOCKS server failure",
    2: "connection not allowed by ruleset",
    3: "network unreachable",
    4: "host unreachable",
    5: "connection refused",
    6: "TTL expired",
    7: "command not supported",
    8: "address type not supported",
}

WIN_CREATE_NEW_PROCESS_GROUP = 0x00000200
WIN_DETACHED_PROCESS = 0x00000008
WIN_CREATE_NO_WINDOW = 0x08000000


def default_state_dir() -> Path:
    return Path.home() / ".tailscale"


def recv_exact(sock: socket.socket, length: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < length:
        chunk = sock.recv(length - len(chunks))
        if not chunk:
            raise EOFError(f"Unexpected EOF after {len(chunks)} of {length} bytes")
        chunks.extend(chunk)
    return bytes(chunks)


def build_socks5_connect_request(host: str, port: int) -> bytes:
    if not (1 <= port <= 65535):
        raise ValueError(f"Invalid port: {port}")

    try:
        ipv4 = socket.inet_pton(socket.AF_INET, host)
        addr_part = b"\x01" + ipv4
    except OSError:
        try:
            ipv6 = socket.inet_pton(socket.AF_INET6, host)
            addr_part = b"\x04" + ipv6
        except OSError:
            encoded = host.encode("utf-8")
            if not encoded or len(encoded) > 255:
                raise ValueError(f"Invalid target host: {host!r}")
            addr_part = b"\x03" + bytes([len(encoded)]) + encoded

    return b"\x05\x01\x00" + addr_part + struct.pack("!H", port)


def socks5_connect(
    socks_host: str,
    socks_port: int,
    target_host: str,
    target_port: int,
    timeout: float = 15.0,
) -> socket.socket:
    proxy = socket.create_connection((socks_host, socks_port), timeout=timeout)
    try:
        proxy.sendall(b"\x05\x01\x00")
        greeting = recv_exact(proxy, 2)
        if greeting != b"\x05\x00":
            raise RuntimeError(f"SOCKS5 auth failed: {greeting!r}")

        proxy.sendall(build_socks5_connect_request(target_host, target_port))
        header = recv_exact(proxy, 4)
        version, reply, _reserved, atyp = header
        if version != 5:
            raise RuntimeError(f"Invalid SOCKS5 reply version: {version}")
        if reply != 0:
            detail = SOCKS5_ERRORS.get(reply, f"code {reply}")
            raise RuntimeError(f"SOCKS5 connect to {target_host}:{target_port} failed: {detail}")

        if atyp == 1:
            recv_exact(proxy, 4 + 2)
        elif atyp == 3:
            domain_len = recv_exact(proxy, 1)[0]
            recv_exact(proxy, domain_len + 2)
        elif atyp == 4:
            recv_exact(proxy, 16 + 2)
        else:
            raise RuntimeError(f"Unsupported SOCKS5 address type: {atyp}")

        proxy.settimeout(None)
        try:
            proxy.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        return proxy
    except Exception:
        proxy.close()
        raise


def pipe_sockets(src: socket.socket, dst: socket.socket) -> None:
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        for sock in (src, dst):
            try:
                sock.close()
            except OSError:
                pass


def handle_client(
    client: socket.socket,
    socks_host: str,
    socks_port: int,
    target_host: str,
    target_port: int,
) -> None:
    try:
        try:
            client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        proxy = socks5_connect(socks_host, socks_port, target_host, target_port)
    except Exception as exc:
        print(f"Connection error: {exc}", file=sys.stderr, flush=True)
        try:
            client.close()
        except OSError:
            pass
        return

    thread = threading.Thread(target=pipe_sockets, args=(client, proxy), daemon=True)
    thread.start()
    pipe_sockets(proxy, client)


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_suffix(path.suffix + f".{os.getpid()}.tmp")
    tmp_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp_path, path)


def read_json_file(path: Path) -> dict[str, Any] | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else None
    except (OSError, json.JSONDecodeError):
        return None


def serve_tunnel(
    listen_host: str,
    listen_port: int,
    socks_host: str,
    socks_port: int,
    target_host: str,
    target_port: int,
    ready_file: Path | None = None,
    stop_event: threading.Event | None = None,
) -> int:
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind((listen_host, listen_port))
        server.listen(16)
        actual_host, actual_port = server.getsockname()
        server.settimeout(0.5)

        if ready_file is not None:
            write_json_atomic(
                ready_file,
                {
                    "pid": os.getpid(),
                    "listen_host": actual_host,
                    "listen_port": actual_port,
                    "target_host": target_host,
                    "target_port": target_port,
                    "socks_host": socks_host,
                    "socks_port": socks_port,
                },
            )

        print(
            f"VNC tunnel ready -> {actual_host}:{actual_port} "
            f"(via SOCKS5 {socks_host}:{socks_port} -> {target_host}:{target_port})",
            flush=True,
        )

        while stop_event is None or not stop_event.is_set():
            try:
                client, _addr = server.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(
                target=handle_client,
                args=(client, socks_host, socks_port, target_host, target_port),
                daemon=True,
            ).start()
    except KeyboardInterrupt:
        return 0
    finally:
        server.close()
        if ready_file is not None:
            existing = read_json_file(ready_file)
            if existing and existing.get("pid") == os.getpid():
                try:
                    ready_file.unlink()
                except OSError:
                    pass
    return 0


def find_tailscale_binaries(tailscale_dir: str | None = None) -> tuple[Path | None, Path | None]:
    candidates: list[Path] = []
    if tailscale_dir:
        candidates.append(Path(tailscale_dir))

    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        candidates.append(Path(local_app_data) / "Programs" / "tailscale")

    candidates.append(Path.home() / ".tailscale" / "app" / "PFiles64" / "Tailscale")

    ts_cli: Path | None = None
    ts_daemon: Path | None = None
    cli_names = ("tailscale.exe", "tailscale")
    daemon_names = ("tailscaled.exe", "tailscaled")

    for directory in candidates:
        if ts_cli is None:
            for name in cli_names:
                path = directory / name
                if path.is_file():
                    ts_cli = path
                    break
        if ts_daemon is None:
            for name in daemon_names:
                path = directory / name
                if path.is_file():
                    ts_daemon = path
                    break

    if ts_cli is None:
        found = shutil.which("tailscale")
        if found:
            ts_cli = Path(found)
    if ts_daemon is None:
        found = shutil.which("tailscaled")
        if found:
            ts_daemon = Path(found)

    return ts_cli, ts_daemon


def find_mac_mirror_viewer(explicit_path: str | None = None) -> Path | None:
    if explicit_path:
        path = Path(explicit_path)
        return path if path.is_file() else None

    candidates: list[Path] = []
    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        candidates.append(Path(local_app_data) / "Programs" / "mac-mirror" / "mac-mirror-viewer.exe")

    script_dir = Path(__file__).resolve().parent
    candidates.append(script_dir / "mac-mirror-viewer.exe")
    if os.name == "nt":
        candidates.append(script_dir / "mac_mirror_viewer" / "build" / "mac-mirror-viewer.exe")

    for candidate in candidates:
        if candidate.is_file():
            return candidate

    for name in ("mac-mirror-viewer.exe", "mac-mirror-viewer"):
        found = shutil.which(name)
        if found:
            return Path(found)
    return None


def find_vncviewer(explicit_path: str | None = None) -> Path | None:
    if explicit_path:
        path = Path(explicit_path)
        return path if path.is_file() else None

    local_app_data = os.environ.get("LOCALAPPDATA")
    if local_app_data:
        for name in ("vncviewer.exe", "vncviewer64-1.16.2.exe"):
            candidate = Path(local_app_data) / "Programs" / "tigervnc" / name
            if candidate.is_file():
                return candidate

    found = shutil.which("vncviewer")
    return Path(found) if found else None


def is_pid_running(pid: int) -> bool:
    if pid <= 0:
        return False
    if os.name == "nt":
        synchronize = 0x00100000
        query_limited = 0x1000
        still_active = 259
        kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
        handle = kernel32.OpenProcess(synchronize | query_limited, False, pid)
        if not handle:
            return False
        try:
            exit_code = ctypes.c_ulong()
            if kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)):
                return exit_code.value == still_active
            return False
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def terminate_pid(pid: int) -> bool:
    if not is_pid_running(pid):
        return False
    if os.name == "nt":
        result = subprocess.run(
            ["taskkill", "/PID", str(pid), "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return result.returncode == 0
    try:
        os.kill(pid, signal.SIGTERM)
        return True
    except OSError:
        return False


def get_tailscale_status(tailscale_exe: Path, socket_path: str) -> dict[str, Any] | None:
    try:
        proc = subprocess.run(
            [str(tailscale_exe), f"--socket={socket_path}", "status", "--json"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None

    stdout = proc.stdout.strip()
    if not stdout:
        return None
    try:
        data = json.loads(stdout)
    except json.JSONDecodeError:
        return None
    if isinstance(data, dict) and "BackendState" in data:
        return data
    return None


def _pick_preferred_ip(ips: list[Any]) -> str | None:
    valid = [ip for ip in ips if isinstance(ip, str) and ip]
    for ip in valid:
        try:
            socket.inet_pton(socket.AF_INET, ip)
            return ip
        except OSError:
            continue
    return valid[0] if valid else None


def resolve_peer_ip(status: dict[str, Any], peer_match: str) -> tuple[str, str, bool] | None:
    peers = status.get("Peer")
    if not isinstance(peers, dict):
        return None

    needle = peer_match.lower()
    for peer in peers.values():
        if not isinstance(peer, dict):
            continue
        hostname = str(peer.get("HostName") or "")
        dns_name = str(peer.get("DNSName") or "")
        if needle in hostname.lower() or needle in dns_name.lower():
            ips = peer.get("TailscaleIPs")
            if isinstance(ips, list):
                ip = _pick_preferred_ip(ips)
                if ip:
                    label = hostname or dns_name.rstrip(".")
                    return ip, label, bool(peer.get("Online", False))
    return None


def list_peer_summaries(status: dict[str, Any]) -> list[str]:
    peers = status.get("Peer")
    if not isinstance(peers, dict):
        return []
    items: list[str] = []
    for peer in peers.values():
        if not isinstance(peer, dict):
            continue
        hostname = str(peer.get("HostName") or peer.get("DNSName") or "unknown")
        ips = peer.get("TailscaleIPs")
        ip = _pick_preferred_ip(ips) if isinstance(ips, list) else None
        items.append(f"{hostname} ({ip or 'no-ip'})")
    return sorted(items)


def spawn_background(cmd: list[str], log_path: Path, gui: bool = False) -> subprocess.Popen[Any]:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    kwargs: dict[str, Any] = {
        "stdin": subprocess.DEVNULL,
        "close_fds": True,
    }
    if os.name == "nt":
        if gui:
            kwargs["creationflags"] = WIN_DETACHED_PROCESS | WIN_CREATE_NEW_PROCESS_GROUP
        else:
            kwargs["creationflags"] = WIN_CREATE_NO_WINDOW | WIN_CREATE_NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True

    if gui:
        kwargs["stdout"] = subprocess.DEVNULL
        kwargs["stderr"] = subprocess.DEVNULL
        return subprocess.Popen(cmd, **kwargs)

    with log_path.open("ab") as log_file:
        kwargs["stdout"] = log_file
        kwargs["stderr"] = subprocess.STDOUT
        return subprocess.Popen(cmd, **kwargs)


def read_log_tail(log_path: Path, max_lines: int = 20) -> str:
    try:
        lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return ""
    return "\n".join(lines[-max_lines:])


def ensure_tailscaled(
    tailscale_exe: Path,
    tailscaled_exe: Path,
    state_dir: Path,
    socket_path: str,
    socks_host: str,
    socks_port: int,
    timeout: float = 15.0,
) -> dict[str, Any]:
    status = get_tailscale_status(tailscale_exe, socket_path)
    if status is not None:
        return status

    state_dir.mkdir(parents=True, exist_ok=True)
    state_file = state_dir / "tailscaled.state"
    log_path = state_dir / "tailscaled.log"
    pid_path = state_dir / "tailscaled.pid"

    cmd = [
        str(tailscaled_exe),
        "--windows-mode=dev",
        "--tun=userspace-networking",
        f"--state={state_file}",
        f"--socket={socket_path}",
        f"--socks5-server={socks_host}:{socks_port}",
    ]
    print(f"Starting Tailscale userspace daemon (SOCKS5 {socks_host}:{socks_port})...", flush=True)
    print(
        "Note: If a Windows Firewall dialog appears without admin rights, clicking Cancel is fine.",
        flush=True,
    )
    proc = spawn_background(cmd, log_path=log_path, gui=False)
    pid_path.write_text(f"{proc.pid}\n", encoding="utf-8")

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = get_tailscale_status(tailscale_exe, socket_path)
        if status is not None:
            return status
        if proc.poll() is not None:
            tail = read_log_tail(log_path)
            raise RuntimeError(
                f"tailscaled exited early with code {proc.returncode}.\n{tail}".strip()
            )
        time.sleep(0.4)

    tail = read_log_tail(log_path)
    raise RuntimeError(
        f"Timed out waiting for tailscaled on {socket_path}.\n{tail}".strip()
    )


def ensure_tailscale_connected(
    tailscale_exe: Path,
    socket_path: str,
    status: dict[str, Any],
    auth_key: str | None = None,
) -> dict[str, Any]:
    if status.get("BackendState") == "Running":
        return status

    cmd = [str(tailscale_exe), f"--socket={socket_path}", "up", "--unattended"]
    if auth_key:
        cmd.append(f"--auth-key={auth_key}")

    print(
        "Connecting Tailscale (--unattended)... Complete sign-in in your browser if prompted.",
        flush=True,
    )
    result = subprocess.run(cmd, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"tailscale up failed with exit code {result.returncode}.")

    deadline = time.monotonic() + 5.0
    refreshed = get_tailscale_status(tailscale_exe, socket_path)
    while (refreshed is None or refreshed.get("BackendState") != "Running") and time.monotonic() < deadline:
        time.sleep(0.3)
        refreshed = get_tailscale_status(tailscale_exe, socket_path)

    if refreshed is None or refreshed.get("BackendState") != "Running":
        state = refreshed.get("BackendState") if refreshed else "unknown"
        raise RuntimeError(f"Tailscale did not reach Running state (current state: {state}).")
    return refreshed


def resolve_target_host(
    tailscale_exe: Path,
    socket_path: str,
    status: dict[str, Any],
    explicit_host: str | None,
    peer_match: str,
    wait_seconds: float = 8.0,
) -> str:
    if explicit_host:
        return explicit_host

    deadline = time.monotonic() + wait_seconds
    current = status
    while True:
        found = resolve_peer_ip(current, peer_match)
        if found is not None:
            ip, label, online = found
            online_note = "online" if online else "offline/idle"
            print(f"Resolved Mac peer: {label} ({ip}, {online_note})", flush=True)
            return ip

        if time.monotonic() >= deadline:
            break
        time.sleep(0.5)
        refreshed = get_tailscale_status(tailscale_exe, socket_path)
        if refreshed is not None:
            current = refreshed

    peers = list_peer_summaries(current)
    peer_hint = f" Visible peers: {', '.join(peers)}." if peers else " No peers are visible yet."
    raise RuntimeError(
        f"Could not find Mac peer matching '{peer_match}' in Tailscale status.{peer_hint} "
        "Accept the Mac Node Share invite link (created via `ts-share create` on Mac) "
        "in your browser, then run `mac-mirror` again (or pass `--host <IP>`)."
    )


def ensure_background_tunnel(
    state_dir: Path,
    listen_host: str,
    listen_port: int,
    socks_host: str,
    socks_port: int,
    target_host: str,
    target_port: int,
    timeout: float = 10.0,
) -> Path:
    ready_file = state_dir / "mac-vnc-tunnel.json"
    log_path = state_dir / "mac-vnc-tunnel.log"
    existing = read_json_file(ready_file)
    if existing:
        pid = int(existing.get("pid", 0) or 0)
        same_config = (
            existing.get("listen_host") == listen_host
            and existing.get("listen_port") == listen_port
            and existing.get("target_host") == target_host
            and existing.get("target_port") == target_port
            and existing.get("socks_host") == socks_host
            and existing.get("socks_port") == socks_port
        )
        if pid > 0 and is_pid_running(pid) and same_config:
            print(
                f"Reusing VNC tunnel on {listen_host}:{listen_port} -> {target_host}:{target_port}",
                flush=True,
            )
            return ready_file
        if pid > 0 and is_pid_running(pid):
            terminate_pid(pid)
            time.sleep(0.3)
        try:
            ready_file.unlink()
        except OSError:
            pass

    cmd = [
        sys.executable,
        str(Path(__file__).resolve()),
        "--tunnel-only",
        "--host",
        target_host,
        "--remote-port",
        str(target_port),
        "--listen-host",
        listen_host,
        "--listen-port",
        str(listen_port),
        "--socks-host",
        socks_host,
        "--socks-port",
        str(socks_port),
        "--ready-file",
        str(ready_file),
    ]
    proc = spawn_background(cmd, log_path=log_path, gui=False)

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        info = read_json_file(ready_file)
        if info and info.get("pid") == proc.pid:
            print(
                f"VNC tunnel ready -> {listen_host}:{listen_port} "
                f"(via SOCKS5 {socks_host}:{socks_port} -> {target_host}:{target_port})",
                flush=True,
            )
            return ready_file
        if proc.poll() is not None:
            tail = read_log_tail(log_path)
            raise RuntimeError(
                f"VNC tunnel exited early with code {proc.returncode}.\n{tail}".strip()
            )
        time.sleep(0.2)

    tail = read_log_tail(log_path)
    raise RuntimeError(f"Timed out waiting for VNC tunnel on {listen_host}:{listen_port}.\n{tail}".strip())


def launch_mac_mirror_viewer(
    viewer_exe: Path,
    target_host: str,
    target_port: int,
    socks_host: str,
    socks_port: int,
    state_dir: Path,
    extra_args: list[str] | None = None,
) -> subprocess.Popen[Any]:
    socks_endpoint = f"{socks_host}:{socks_port}"
    cmd = [
        str(viewer_exe),
        "--host",
        target_host,
        "--port",
        str(target_port),
        "--socks5",
        socks_endpoint,
    ]
    if extra_args:
        cmd.extend(extra_args)
    state_dir.mkdir(parents=True, exist_ok=True)
    proc = spawn_background(cmd, log_path=state_dir / "mac-mirror-viewer.log", gui=True)
    try:
        (state_dir / "mac-mirror-viewer.pid").write_text(f"{proc.pid}\n", encoding="utf-8")
    except OSError:
        pass
    print(
        f"Mac Mirror Viewer launched -> {target_host}:{target_port} "
        f"(direct SOCKS5 {socks_endpoint}). Sign in with your Mac account or VNC password.",
        flush=True,
    )
    print("Before leaving a shared PC, run: mac-mirror-stop --logout", flush=True)
    return proc


def launch_vncviewer(
    vncviewer_exe: Path,
    listen_host: str,
    listen_port: int,
    state_dir: Path,
    extra_args: list[str] | None = None,
) -> subprocess.Popen[Any]:
    cmd = [str(vncviewer_exe), "-DotWhenNoCursor=1"]
    if extra_args:
        cmd.extend(extra_args)
    cmd.append(f"{listen_host}:{listen_port}")
    proc = spawn_background(cmd, log_path=state_dir / "vncviewer.log", gui=True)
    print(
        f"TigerVNC launched -> {listen_host}:{listen_port} (-DotWhenNoCursor=1). "
        "Sign in with your Mac account or VNC password.",
        flush=True,
    )
    print("Before leaving a shared PC, run: mac-mirror-stop --logout", flush=True)
    return proc


def cmd_connect(args: argparse.Namespace) -> int:
    state_dir = Path(args.state_dir)
    tailscale_exe, tailscaled_exe = find_tailscale_binaries(args.tailscale_dir)
    if tailscale_exe is None or tailscaled_exe is None:
        raise RuntimeError(
            "Tailscale userspace binaries (tailscale.exe / tailscaled.exe) were not found. "
            "Run computer_setting.ps1 first."
        )

    native_viewer_exe: Path | None = None
    vncviewer_exe: Path | None = None
    if not args.no_viewer:
        if args.viewer:
            native_viewer_exe = find_mac_mirror_viewer(args.viewer)
            if native_viewer_exe is None:
                raise RuntimeError(
                    f"Mac Mirror Viewer ({args.viewer}) was not found. Run computer_setting.ps1 first."
                )
        elif args.vncviewer:
            if Path(args.vncviewer).name.lower().startswith("mac-mirror-viewer"):
                native_viewer_exe = find_mac_mirror_viewer(args.vncviewer)
                if native_viewer_exe is None:
                    raise RuntimeError(
                        f"Mac Mirror Viewer ({args.vncviewer}) was not found. Run computer_setting.ps1 first."
                    )
            else:
                vncviewer_exe = find_vncviewer(args.vncviewer)
                if vncviewer_exe is None:
                    raise RuntimeError(
                        "TigerVNC Viewer (vncviewer.exe) was not found. Run computer_setting.ps1 first."
                    )
        elif not args.force_tigervnc:
            native_viewer_exe = find_mac_mirror_viewer(None)
            if native_viewer_exe is None:
                vncviewer_exe = find_vncviewer(None)
        else:
            vncviewer_exe = find_vncviewer(None)

        if native_viewer_exe is None and vncviewer_exe is None:
            raise RuntimeError(
                "Neither mac-mirror-viewer.exe nor TigerVNC Viewer (vncviewer.exe) was found. "
                "Run computer_setting.ps1 first."
            )

    status = ensure_tailscaled(
        tailscale_exe=tailscale_exe,
        tailscaled_exe=tailscaled_exe,
        state_dir=state_dir,
        socket_path=args.socket,
        socks_host=args.socks_host,
        socks_port=args.socks_port,
    )
    status = ensure_tailscale_connected(
        tailscale_exe=tailscale_exe,
        socket_path=args.socket,
        status=status,
        auth_key=args.auth_key,
    )
    target_host = resolve_target_host(
        tailscale_exe=tailscale_exe,
        socket_path=args.socket,
        status=status,
        explicit_host=args.host,
        peer_match=args.peer,
    )

    if native_viewer_exe is not None:
        native_extra_args: list[str] = []
        if args.windowed:
            native_extra_args.append("--windowed")
        if args.cursor_scale is not None:
            native_extra_args.extend(["--cursor-scale", str(args.cursor_scale)])
        if args.quality:
            native_extra_args.extend(["--quality", args.quality])
        if args.viewer_arg:
            native_extra_args.extend(args.viewer_arg)
        launch_mac_mirror_viewer(
            viewer_exe=native_viewer_exe,
            target_host=target_host,
            target_port=args.remote_port,
            socks_host=args.socks_host,
            socks_port=args.socks_port,
            state_dir=state_dir,
            extra_args=native_extra_args,
        )
        return 0

    ensure_background_tunnel(
        state_dir=state_dir,
        listen_host=args.listen_host,
        listen_port=args.listen_port,
        socks_host=args.socks_host,
        socks_port=args.socks_port,
        target_host=target_host,
        target_port=args.remote_port,
    )
    if vncviewer_exe is not None:
        launch_vncviewer(
            vncviewer_exe=vncviewer_exe,
            listen_host=args.listen_host,
            listen_port=args.listen_port,
            state_dir=state_dir,
            extra_args=args.viewer_arg,
        )
    return 0


def cmd_stop(args: argparse.Namespace) -> int:
    state_dir = Path(args.state_dir)
    viewer_pid_file = state_dir / "mac-mirror-viewer.pid"
    ready_file = state_dir / "mac-vnc-tunnel.json"
    pid_file = state_dir / "tailscaled.pid"
    state_file = state_dir / "tailscaled.state"

    if viewer_pid_file.is_file():
        try:
            viewer_pid = int(viewer_pid_file.read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            viewer_pid = 0
        if viewer_pid > 0 and terminate_pid(viewer_pid):
            print(f"Stopped Mac Mirror Viewer (PID {viewer_pid}).", flush=True)
        try:
            viewer_pid_file.unlink()
        except OSError:
            pass

    tunnel_info = read_json_file(ready_file)
    if tunnel_info:
        pid = int(tunnel_info.get("pid", 0) or 0)
        if pid > 0 and terminate_pid(pid):
            print(f"Stopped VNC tunnel (PID {pid}).", flush=True)
        try:
            ready_file.unlink()
        except OSError:
            pass

    tailscale_exe, _ = find_tailscale_binaries(args.tailscale_dir)
    if tailscale_exe is not None and get_tailscale_status(tailscale_exe, args.socket) is not None:
        action = "logout" if args.logout else "down"
        result = subprocess.run(
            [str(tailscale_exe), f"--socket={args.socket}", action],
            check=False,
        )
        if result.returncode == 0:
            print(f"Tailscale {action} completed.", flush=True)
        else:
            print(
                f"Warning: tailscale {action} exited with code {result.returncode}.",
                file=sys.stderr,
                flush=True,
            )

    if pid_file.is_file():
        try:
            ts_pid = int(pid_file.read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            ts_pid = 0
        if ts_pid > 0 and terminate_pid(ts_pid):
            print(f"Stopped tailscaled (PID {ts_pid}).", flush=True)
        try:
            pid_file.unlink()
        except OSError:
            pass

    if args.logout and state_file.is_file():
        try:
            state_file.unlink()
            print(f"Removed Tailscale state file: {state_file}", flush=True)
        except OSError as exc:
            print(f"Warning: could not remove {state_file}: {exc}", file=sys.stderr, flush=True)

    return 0


def cmd_status(args: argparse.Namespace) -> int:
    state_dir = Path(args.state_dir)
    tailscale_exe, tailscaled_exe = find_tailscale_binaries(args.tailscale_dir)
    native_viewer_exe = find_mac_mirror_viewer(args.viewer)
    vncviewer_exe = find_vncviewer(args.vncviewer)

    print(f"tailscale.exe        : {tailscale_exe or 'missing'}")
    print(f"tailscaled.exe       : {tailscaled_exe or 'missing'}")
    print(f"mac-mirror-viewer.exe: {native_viewer_exe or 'missing'}")
    print(f"vncviewer.exe        : {vncviewer_exe or 'missing'}")

    status = get_tailscale_status(tailscale_exe, args.socket) if tailscale_exe else None
    if status is None:
        print("Tailscale            : stopped")
    else:
        backend = status.get("BackendState", "unknown")
        self_info = status.get("Self") if isinstance(status.get("Self"), dict) else {}
        self_ips = self_info.get("TailscaleIPs") if isinstance(self_info, dict) else []
        self_ip = _pick_preferred_ip(self_ips) if isinstance(self_ips, list) else None
        print(f"Tailscale            : {backend} ({self_ip or 'no-ip'})")
        peer = resolve_peer_ip(status, args.peer)
        if peer:
            print(f"Mac peer             : {peer[1]} ({peer[0]}, {'online' if peer[2] else 'offline/idle'})")
        else:
            print(f"Mac peer             : not found (filter: {args.peer})")

    tunnel_info = read_json_file(state_dir / "mac-vnc-tunnel.json")
    if tunnel_info and is_pid_running(int(tunnel_info.get("pid", 0) or 0)):
        print(
            f"VNC tunnel           : running (PID {tunnel_info['pid']}) "
            f"{tunnel_info['listen_host']}:{tunnel_info['listen_port']} -> "
            f"{tunnel_info['target_host']}:{tunnel_info['target_port']}"
        )
    else:
        print("VNC tunnel           : stopped")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Tailscale userspace SOCKS5 VNC tunnel and one-shot Mac screen mirroring launcher."
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--connect",
        action="store_true",
        help=(
            "One-shot connect: start tailscaled, run tailscale up --unattended, "
            "and launch mac-mirror-viewer.exe directly over SOCKS5 (or fallback to TigerVNC)."
        ),
    )
    mode.add_argument(
        "--stop",
        action="store_true",
        help="Stop the active viewer, background VNC tunnel, and Tailscale userspace daemon.",
    )
    mode.add_argument(
        "--status",
        action="store_true",
        help="Print current Tailscale, Mac peer, VNC tunnel, and viewer status.",
    )
    mode.add_argument(
        "--tunnel-only",
        action="store_true",
        help="Run only the foreground SOCKS5-to-VNC TCP tunnel (default if no mode flag is passed).",
    )

    parser.add_argument("--host", default=None, help="Explicit Mac Tailscale IP or hostname.")
    parser.add_argument(
        "--peer",
        default=DEFAULT_PEER_MATCH,
        help=f"Peer hostname/DNS substring to auto-resolve (default: {DEFAULT_PEER_MATCH}).",
    )
    parser.add_argument("--remote-port", type=int, default=DEFAULT_REMOTE_PORT)
    parser.add_argument("--listen-host", default=DEFAULT_LISTEN_HOST)
    parser.add_argument("--listen-port", type=int, default=DEFAULT_LISTEN_PORT)
    parser.add_argument("--socks-host", default=DEFAULT_SOCKS_HOST)
    parser.add_argument("--socks-port", type=int, default=DEFAULT_SOCKS_PORT)
    parser.add_argument("--socket", default=DEFAULT_PIPE_SOCKET)
    parser.add_argument("--state-dir", default=str(default_state_dir()))
    parser.add_argument("--tailscale-dir", default=None)
    parser.add_argument("--viewer", default=None, help="Explicit path to mac-mirror-viewer.exe.")
    parser.add_argument("--vncviewer", default=None, help="Explicit path to fallback vncviewer.exe.")
    parser.add_argument(
        "--force-tigervnc",
        action="store_true",
        help="Force local TCP forwarder + TigerVNC fallback even when mac-mirror-viewer.exe is installed.",
    )
    parser.add_argument("--auth-key", default=None, help="Optional Tailscale auth key for tailscale up.")
    parser.add_argument("--no-viewer", action="store_true", help="Do not launch viewer after setup.")
    parser.add_argument("--windowed", action="store_true", help="Launch mac-mirror-viewer.exe in windowed mode.")
    parser.add_argument(
        "--cursor-scale",
        type=float,
        default=None,
        help="Vector macOS cursor scale (1.0..4.0) for mac-mirror-viewer.exe.",
    )
    parser.add_argument(
        "--quality",
        choices=("adaptive", "sharp", "turbo"),
        default=None,
        help="Quality mode for mac-mirror-viewer.exe.",
    )
    parser.add_argument(
        "--viewer-arg",
        action="append",
        default=[],
        help="Additional argument passed to the viewer executable (repeatable, e.g. --viewer-arg=--flag).",
    )
    parser.add_argument("--logout", action="store_true", help="With --stop: run tailscale logout and delete state.")
    parser.add_argument("--ready-file", default=None, help="Internal: JSON file written once tunnel is listening.")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        if args.connect:
            return cmd_connect(args)
        if args.stop:
            return cmd_stop(args)
        if args.status:
            return cmd_status(args)

        target_host = args.host
        if not target_host:
            tailscale_exe, _ = find_tailscale_binaries(args.tailscale_dir)
            if tailscale_exe is not None:
                status = get_tailscale_status(tailscale_exe, args.socket)
                if status is not None:
                    found = resolve_peer_ip(status, args.peer)
                    if found is not None:
                        target_host = found[0]
            target_host = target_host or DEFAULT_MAC_HOST

        ready_file = Path(args.ready_file) if args.ready_file else None
        return serve_tunnel(
            listen_host=args.listen_host,
            listen_port=args.listen_port,
            socks_host=args.socks_host,
            socks_port=args.socks_port,
            target_host=target_host,
            target_port=args.remote_port,
            ready_file=ready_file,
        )
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
