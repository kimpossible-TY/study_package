import json
import socket
import struct
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

import mac_vnc_tunnel as tunnel  # noqa: E402


class _MockVncServer:
    """Minimal TCP server mimicking macOS Screen Sharing (server speaks first)."""

    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(5)
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.stop_event.set()
        self.sock.close()
        self.thread.join(timeout=2)

    def _run(self) -> None:
        while not self.stop_event.is_set():
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    @staticmethod
    def _handle(conn: socket.socket) -> None:
        try:
            conn.sendall(b"RFB 003.889\n")
            data = conn.recv(1024)
            if data:
                conn.sendall(b"ECHO:" + data)
        except OSError:
            pass
        finally:
            conn.close()


class _MockSocks5Server:
    """Minimal RFC 1928 SOCKS5 server forwarding connections to a target server."""

    def __init__(self, reply_code: int = 0) -> None:
        self.reply_code = reply_code
        self.last_target: tuple[str, int] | None = None
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(5)
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.stop_event.set()
        self.sock.close()
        self.thread.join(timeout=2)

    def _run(self) -> None:
        while not self.stop_event.is_set():
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _handle(self, client: socket.socket) -> None:
        upstream: socket.socket | None = None
        try:
            greeting = tunnel.recv_exact(client, 2)
            nmethods = greeting[1]
            tunnel.recv_exact(client, nmethods)
            client.sendall(b"\x05\x00")

            header = tunnel.recv_exact(client, 4)
            atyp = header[3]
            if atyp == 1:
                host = socket.inet_ntop(socket.AF_INET, tunnel.recv_exact(client, 4))
            elif atyp == 3:
                dlen = tunnel.recv_exact(client, 1)[0]
                host = tunnel.recv_exact(client, dlen).decode("utf-8")
            elif atyp == 4:
                host = socket.inet_ntop(socket.AF_INET6, tunnel.recv_exact(client, 16))
            else:
                client.sendall(b"\x05\x08\x00\x01\x00\x00\x00\x00\x00\x00")
                return
            port = struct.unpack("!H", tunnel.recv_exact(client, 2))[0]
            self.last_target = (host, port)

            if self.reply_code != 0:
                client.sendall(bytes([5, self.reply_code, 0, 1, 0, 0, 0, 0, 0, 0]))
                client.close()
                return

            upstream = socket.create_connection((host, port), timeout=5)
            client.sendall(b"\x05\x00\x00\x01\x7f\x00\x00\x01" + struct.pack("!H", port))

            t = threading.Thread(target=tunnel.pipe_sockets, args=(client, upstream), daemon=True)
            t.start()
            tunnel.pipe_sockets(upstream, client)
        except Exception:
            for s in (client, upstream):
                if s is not None:
                    try:
                        s.close()
                    except OSError:
                        pass


class Socks5AndTunnelTests(unittest.TestCase):
    def test_build_socks5_connect_request_variants(self) -> None:
        req_v4 = tunnel.build_socks5_connect_request("100.80.74.94", 5900)
        self.assertEqual(req_v4[:4], b"\x05\x01\x00\x01")
        self.assertEqual(req_v4[4:8], socket.inet_aton("100.80.74.94"))
        self.assertEqual(struct.unpack("!H", req_v4[8:])[0], 5900)

        req_domain = tunnel.build_socks5_connect_request("mac.ts.net", 5900)
        self.assertEqual(req_domain[:4], b"\x05\x01\x00\x03")
        self.assertEqual(req_domain[4], len("mac.ts.net"))
        self.assertEqual(req_domain[5:-2], b"mac.ts.net")

        with self.assertRaises(ValueError):
            tunnel.build_socks5_connect_request("100.80.74.94", 0)

    def test_end_to_end_vnc_tunnel_over_socks5(self) -> None:
        vnc = _MockVncServer()
        socks = _MockSocks5Server()
        vnc.start()
        socks.start()
        stop_event = threading.Event()
        try:
            with tempfile.TemporaryDirectory() as tmp:
                ready_file = Path(tmp) / "tunnel.json"
                server_thread = threading.Thread(
                    target=tunnel.serve_tunnel,
                    kwargs={
                        "listen_host": "127.0.0.1",
                        "listen_port": 0,
                        "socks_host": "127.0.0.1",
                        "socks_port": socks.port,
                        "target_host": "127.0.0.1",
                        "target_port": vnc.port,
                        "ready_file": ready_file,
                        "stop_event": stop_event,
                    },
                    daemon=True,
                )
                server_thread.start()

                deadline = time.monotonic() + 5.0
                info = None
                while time.monotonic() < deadline:
                    info = tunnel.read_json_file(ready_file)
                    if info is not None:
                        break
                    time.sleep(0.05)

                self.assertIsNotNone(info)
                assert info is not None
                listen_port = int(info["listen_port"])

                with socket.create_connection(("127.0.0.1", listen_port), timeout=5) as client:
                    banner = tunnel.recv_exact(client, len(b"RFB 003.889\n"))
                    self.assertEqual(banner, b"RFB 003.889\n")
                    client.sendall(b"RFB 003.889\n")
                    reply = tunnel.recv_exact(client, len(b"ECHO:RFB 003.889\n"))
                    self.assertEqual(reply, b"ECHO:RFB 003.889\n")

                self.assertEqual(socks.last_target, ("127.0.0.1", vnc.port))

                stop_event.set()
                server_thread.join(timeout=3)
                self.assertFalse(ready_file.exists())
        finally:
            stop_event.set()
            socks.stop()
            vnc.stop()

    def test_socks5_connect_error_surfaces_reason(self) -> None:
        socks = _MockSocks5Server(reply_code=5)
        socks.start()
        try:
            with self.assertRaisesRegex(RuntimeError, "connection refused"):
                tunnel.socks5_connect("127.0.0.1", socks.port, "100.80.74.94", 5900, timeout=5)
        finally:
            socks.stop()


class PeerResolutionAndOrchestrationTests(unittest.TestCase):
    def test_resolve_peer_ip_prefers_ipv4_and_matches_hostname_or_dns(self) -> None:
        status = {
            "BackendState": "Running",
            "Peer": {
                "nodekey:1": {
                    "HostName": "other-device",
                    "DNSName": "other-device.tail8adc61.ts.net.",
                    "TailscaleIPs": ["100.70.1.2"],
                    "Online": True,
                },
                "nodekey:2": {
                    "HostName": "kimtaeyoungs-macbook-air-daemon",
                    "DNSName": "kimtaeyoungs-macbook-air-daemon.tail8adc61.ts.net.",
                    "TailscaleIPs": ["fd7a:115c:a1e0::a432:4a5f", "100.99.12.34"],
                    "Online": True,
                },
            },
        }
        resolved = tunnel.resolve_peer_ip(status, "kimtaeyoungs-macbook-air")
        self.assertEqual(resolved, ("100.99.12.34", "kimtaeyoungs-macbook-air-daemon", True))
        self.assertIsNone(tunnel.resolve_peer_ip(status, "nonexistent-host"))

    def test_cmd_connect_one_shot_starts_daemon_logs_in_tunnels_and_launches_viewer(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            state_dir = root / ".tailscale"
            ts_dir = root / "tailscale"
            ts_dir.mkdir()
            (ts_dir / "tailscale.exe").write_text("stub", encoding="utf-8")
            (ts_dir / "tailscaled.exe").write_text("stub", encoding="utf-8")
            viewer = root / "vncviewer.exe"
            viewer.write_text("stub", encoding="utf-8")

            status_sequence = [
                None,
                {"BackendState": "NeedsLogin", "Peer": {}},
                {
                    "BackendState": "Running",
                    "Peer": {
                        "p1": {
                            "HostName": "kimtaeyoungs-macbook-air-daemon",
                            "DNSName": "kimtaeyoungs-macbook-air-daemon.tail8adc61.ts.net.",
                            "TailscaleIPs": ["100.80.74.94"],
                            "Online": True,
                        }
                    },
                },
            ]
            spawned_cmds: list[tuple[list[str], bool]] = []
            run_cmds: list[list[str]] = []

            class DummyProc:
                def __init__(self, pid: int) -> None:
                    self.pid = pid
                    self.returncode = None

                def poll(self):
                    return None

            def fake_spawn(cmd: list[str], log_path: Path, gui: bool = False):
                spawned_cmds.append((cmd, gui))
                proc = DummyProc(pid=4000 + len(spawned_cmds))
                if "--tunnel-only" in cmd:
                    ready_idx = cmd.index("--ready-file") + 1
                    tunnel.write_json_atomic(
                        Path(cmd[ready_idx]),
                        {
                            "pid": proc.pid,
                            "listen_host": "127.0.0.1",
                            "listen_port": 15900,
                            "target_host": "100.80.74.94",
                            "target_port": 5900,
                            "socks_host": "127.0.0.1",
                            "socks_port": 1055,
                        },
                    )
                return proc

            def fake_run(cmd: list[str], **kwargs):
                run_cmds.append(cmd)
                return mock.Mock(returncode=0)

            with (
                mock.patch.object(tunnel, "get_tailscale_status", side_effect=status_sequence),
                mock.patch.object(tunnel, "spawn_background", side_effect=fake_spawn),
                mock.patch.object(tunnel.subprocess, "run", side_effect=fake_run),
            ):
                exit_code = tunnel.main(
                    [
                        "--connect",
                        "--state-dir",
                        str(state_dir),
                        "--tailscale-dir",
                        str(ts_dir),
                        "--vncviewer",
                        str(viewer),
                    ]
                )

            self.assertEqual(exit_code, 0)
            self.assertEqual(len(spawned_cmds), 3)

            tailscaled_cmd, tailscaled_gui = spawned_cmds[0]
            self.assertFalse(tailscaled_gui)
            self.assertIn("--windows-mode=dev", tailscaled_cmd)
            self.assertIn("--tun=userspace-networking", tailscaled_cmd)
            self.assertIn("--socks5-server=127.0.0.1:1055", tailscaled_cmd)

            self.assertEqual(len(run_cmds), 1)
            self.assertIn("up", run_cmds[0])
            self.assertIn("--unattended", run_cmds[0])

            tunnel_cmd, tunnel_gui = spawned_cmds[1]
            self.assertFalse(tunnel_gui)
            self.assertIn("--tunnel-only", tunnel_cmd)
            self.assertIn("100.80.74.94", tunnel_cmd)

            viewer_cmd, viewer_gui = spawned_cmds[2]
            self.assertTrue(viewer_gui)
            self.assertEqual(viewer_cmd, [str(viewer), "-DotWhenNoCursor=1", "127.0.0.1:15900"])

    def test_cmd_stop_with_logout_cleans_up_state_and_processes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            state_dir = root / ".tailscale"
            state_dir.mkdir()
            ts_dir = root / "tailscale"
            ts_dir.mkdir()
            (ts_dir / "tailscale.exe").write_text("stub", encoding="utf-8")
            (ts_dir / "tailscaled.exe").write_text("stub", encoding="utf-8")

            ready_file = state_dir / "mac-vnc-tunnel.json"
            tunnel.write_json_atomic(ready_file, {"pid": 11111})
            pid_file = state_dir / "tailscaled.pid"
            pid_file.write_text("22222\n", encoding="utf-8")
            state_file = state_dir / "tailscaled.state"
            state_file.write_text("secret-state", encoding="utf-8")

            terminated: list[int] = []
            run_cmds: list[list[str]] = []

            with (
                mock.patch.object(tunnel, "terminate_pid", side_effect=lambda p: terminated.append(p) or True),
                mock.patch.object(tunnel, "get_tailscale_status", return_value={"BackendState": "Running"}),
                mock.patch.object(
                    tunnel.subprocess,
                    "run",
                    side_effect=lambda cmd, **kw: run_cmds.append(cmd) or mock.Mock(returncode=0),
                ),
            ):
                exit_code = tunnel.main(
                    [
                        "--stop",
                        "--logout",
                        "--state-dir",
                        str(state_dir),
                        "--tailscale-dir",
                        str(ts_dir),
                    ]
                )

            self.assertEqual(exit_code, 0)
            self.assertEqual(terminated, [11111, 22222])
            self.assertEqual(len(run_cmds), 1)
            self.assertEqual(run_cmds[0][-1], "logout")
            self.assertFalse(ready_file.exists())
            self.assertFalse(pid_file.exists())
            self.assertFalse(state_file.exists())

    def test_cmd_connect_reuses_running_daemon_and_tunnel(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            state_dir = root / ".tailscale"
            state_dir.mkdir()
            ts_dir = root / "tailscale"
            ts_dir.mkdir()
            (ts_dir / "tailscale.exe").write_text("stub", encoding="utf-8")
            (ts_dir / "tailscaled.exe").write_text("stub", encoding="utf-8")
            viewer = root / "vncviewer.exe"
            viewer.write_text("stub", encoding="utf-8")

            ready_file = state_dir / "mac-vnc-tunnel.json"
            tunnel.write_json_atomic(
                ready_file,
                {
                    "pid": 54321,
                    "listen_host": "127.0.0.1",
                    "listen_port": 15900,
                    "target_host": "100.80.74.94",
                    "target_port": 5900,
                    "socks_host": "127.0.0.1",
                    "socks_port": 1055,
                },
            )

            running_status = {
                "BackendState": "Running",
                "Peer": {
                    "p1": {
                        "HostName": "kimtaeyoungs-macbook-air-daemon",
                        "DNSName": "kimtaeyoungs-macbook-air-daemon.tail8adc61.ts.net.",
                        "TailscaleIPs": ["100.80.74.94"],
                        "Online": True,
                    }
                },
            }
            spawned: list[tuple[list[str], bool]] = []

            with (
                mock.patch.object(tunnel, "get_tailscale_status", return_value=running_status),
                mock.patch.object(tunnel, "is_pid_running", return_value=True),
                mock.patch.object(
                    tunnel,
                    "spawn_background",
                    side_effect=lambda cmd, log_path, gui=False: spawned.append((cmd, gui)) or mock.Mock(pid=9999),
                ),
            ):
                exit_code = tunnel.main(
                    [
                        "--connect",
                        "--state-dir",
                        str(state_dir),
                        "--tailscale-dir",
                        str(ts_dir),
                        "--vncviewer",
                        str(viewer),
                    ]
                )

            self.assertEqual(exit_code, 0)
            self.assertEqual(len(spawned), 1)
            self.assertEqual(spawned[0], ([str(viewer), "-DotWhenNoCursor=1", "127.0.0.1:15900"], True))

    def test_cmd_connect_prefers_mac_mirror_viewer_directly_without_tcp_relay_hop(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            state_dir = root / ".tailscale"
            local_appdata = root / "LocalAppData"
            viewer_dir = local_appdata / "Programs" / "mac-mirror"
            viewer_dir.mkdir(parents=True)
            native_viewer = viewer_dir / "mac-mirror-viewer.exe"
            native_viewer.write_text("stub-pe", encoding="utf-8")

            tiger_dir = local_appdata / "Programs" / "tigervnc"
            tiger_dir.mkdir(parents=True)
            (tiger_dir / "vncviewer.exe").write_text("stub-tiger", encoding="utf-8")

            ts_dir = root / "tailscale"
            ts_dir.mkdir()
            (ts_dir / "tailscale.exe").write_text("stub", encoding="utf-8")
            (ts_dir / "tailscaled.exe").write_text("stub", encoding="utf-8")

            running_status = {
                "BackendState": "Running",
                "Peer": {
                    "p1": {
                        "HostName": "kimtaeyoungs-macbook-air-daemon",
                        "DNSName": "kimtaeyoungs-macbook-air-daemon.tail8adc61.ts.net.",
                        "TailscaleIPs": ["100.80.74.94"],
                        "Online": True,
                    }
                },
            }
            spawned: list[tuple[list[str], bool]] = []

            with (
                mock.patch.dict(tunnel.os.environ, {"LOCALAPPDATA": str(local_appdata)}, clear=False),
                mock.patch.object(tunnel, "get_tailscale_status", return_value=running_status),
                mock.patch.object(
                    tunnel,
                    "spawn_background",
                    side_effect=lambda cmd, log_path, gui=False: spawned.append((cmd, gui)) or mock.Mock(pid=7777),
                ),
            ):
                exit_code = tunnel.main(
                    [
                        "--connect",
                        "--state-dir",
                        str(state_dir),
                        "--tailscale-dir",
                        str(ts_dir),
                        "--cursor-scale",
                        "2.4",
                        "--viewer-arg=--show-hud",
                    ]
                )

            self.assertEqual(exit_code, 0)
            # Only mac-mirror-viewer.exe is spawned — NO --tunnel-only Python relay process!
            self.assertEqual(len(spawned), 1)
            viewer_cmd, viewer_gui = spawned[0]
            self.assertTrue(viewer_gui)
            self.assertEqual(
                viewer_cmd,
                [
                    str(native_viewer),
                    "--host",
                    "100.80.74.94",
                    "--port",
                    "5900",
                    "--socks5",
                    "127.0.0.1:1055",
                    "--cursor-scale",
                    "2.4",
                    "--show-hud",
                ],
            )
            self.assertEqual((state_dir / "mac-mirror-viewer.pid").read_text(encoding="utf-8").strip(), "7777")

            # Verify --stop terminates the active mac-mirror-viewer PID cleanly too.
            terminated: list[int] = []
            with (
                mock.patch.object(tunnel, "terminate_pid", side_effect=lambda p: terminated.append(p) or True),
                mock.patch.object(tunnel, "get_tailscale_status", return_value=None),
            ):
                stop_code = tunnel.main(
                    [
                        "--stop",
                        "--state-dir",
                        str(state_dir),
                        "--tailscale-dir",
                        str(ts_dir),
                    ]
                )
            self.assertEqual(stop_code, 0)
            self.assertEqual(terminated, [7777])
            self.assertFalse((state_dir / "mac-mirror-viewer.pid").exists())

    def test_cmd_connect_falls_back_to_tigervnc_when_native_viewer_absent_or_forced(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            state_dir = root / ".tailscale"
            local_appdata = root / "LocalAppData"
            viewer_dir = local_appdata / "Programs" / "mac-mirror"
            viewer_dir.mkdir(parents=True)
            (viewer_dir / "mac-mirror-viewer.exe").write_text("stub-pe", encoding="utf-8")

            tiger_dir = local_appdata / "Programs" / "tigervnc"
            tiger_dir.mkdir(parents=True)
            tiger_exe = tiger_dir / "vncviewer.exe"
            tiger_exe.write_text("stub-tiger", encoding="utf-8")

            ts_dir = root / "tailscale"
            ts_dir.mkdir()
            (ts_dir / "tailscale.exe").write_text("stub", encoding="utf-8")
            (ts_dir / "tailscaled.exe").write_text("stub", encoding="utf-8")

            running_status = {
                "BackendState": "Running",
                "Peer": {
                    "p1": {
                        "HostName": "kimtaeyoungs-macbook-air-daemon",
                        "DNSName": "kimtaeyoungs-macbook-air-daemon.tail8adc61.ts.net.",
                        "TailscaleIPs": ["100.80.74.94"],
                        "Online": True,
                    }
                },
            }
            spawned: list[tuple[list[str], bool]] = []

            def fake_spawn(cmd: list[str], log_path: Path, gui: bool = False):
                spawned.append((cmd, gui))
                proc = mock.Mock(pid=8000 + len(spawned), poll=lambda: None)
                if "--tunnel-only" in cmd:
                    ready_idx = cmd.index("--ready-file") + 1
                    tunnel.write_json_atomic(
                        Path(cmd[ready_idx]),
                        {
                            "pid": proc.pid,
                            "listen_host": "127.0.0.1",
                            "listen_port": 15900,
                            "target_host": "100.80.74.94",
                            "target_port": 5900,
                            "socks_host": "127.0.0.1",
                            "socks_port": 1055,
                        },
                    )
                return proc

            with (
                mock.patch.dict(tunnel.os.environ, {"LOCALAPPDATA": str(local_appdata)}, clear=False),
                mock.patch.object(tunnel, "get_tailscale_status", return_value=running_status),
                mock.patch.object(tunnel, "spawn_background", side_effect=fake_spawn),
            ):
                exit_code = tunnel.main(
                    [
                        "--connect",
                        "--force-tigervnc",
                        "--state-dir",
                        str(state_dir),
                        "--tailscale-dir",
                        str(ts_dir),
                    ]
                )

            self.assertEqual(exit_code, 0)
            self.assertEqual(len(spawned), 2)
            self.assertIn("--tunnel-only", spawned[0][0])
            self.assertEqual(spawned[1], ([str(tiger_exe), "-DotWhenNoCursor=1", "127.0.0.1:15900"], True))


class BootstrapScriptContractTests(unittest.TestCase):
    def test_computer_setting_contains_expected_mirroring_setup(self) -> None:
        raw_bytes = (REPO_ROOT / "computer_setting.ps1").read_bytes()
        self.assertFalse(raw_bytes.startswith(b"\xef\xbb\xbf"), "UTF-8 BOM breaks irm | iex")
        script = raw_bytes.decode("utf-8")
        self.assertIn("mac-mirror-viewer.exe", script)
        self.assertIn("function Install-MacMirrorViewer", script)
        self.assertIn(
            "https://sourceforge.net/projects/tigervnc/files/stable/1.16.2/vncviewer64-1.16.2.exe/download",
            script,
        )
        self.assertIn('-UserAgent "curl/8.0"', script)
        self.assertIn("function Install-TailscaleUserspace", script)
        self.assertIn("function Install-TigerVncViewer", script)
        self.assertIn("function Install-MacMirrorTool", script)
        self.assertIn("function global:mac-mirror", script)
        self.assertIn("function global:mac-mirror-stop", script)
        self.assertIn("mac_vnc_tunnel.py", script)
        self.assertIn("[switch]$Update", script)
        self.assertIn("skipping download", script)
        self.assertIn("skipping scoop update", script)
        self.assertIn("msedge.exe", script)
        self.assertIn("socks5://127.0.0.1:1055", script)
        self.assertIn("https://kimtaeyoungs-macbook-air-daemon.tail8adc61.ts.net:8769/", script)

        workflow = (REPO_ROOT / ".github" / "workflows" / "mac-mirror-viewer.yml").read_text(encoding="utf-8")
        self.assertIn("mac-mirror-viewer.exe", workflow)
        self.assertIn("objdump.exe", workflow)
        self.assertIn("-p $exe", workflow)


if __name__ == "__main__":
    unittest.main()
