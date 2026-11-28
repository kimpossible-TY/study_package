"""Automated unittest runner for the C++17 RFB core client engine (`test_rfb_core.cpp`)."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
CORE_DIR = REPO_ROOT / "mac_mirror_viewer" / "core_client"
INC_DIR = REPO_ROOT / "mac_mirror_viewer" / "include"
WIN_UI_DIR = REPO_ROOT / "mac_mirror_viewer" / "win_ui"
TEST_DIR = Path(__file__).resolve().parent
TEST_CPP = TEST_DIR / "test_rfb_core.cpp"


class TestRfbCoreSuite(unittest.TestCase):
    def _run_windows_native_suite(self, tmp_path: Path) -> None:
        cmake = shutil.which("cmake")
        ctest = shutil.which("ctest")
        if cmake and ctest:
            build_dir = tmp_path / "cmake_build"
            cfg_proc = subprocess.run(
                [cmake, "-S", str(TEST_DIR), "-B", str(build_dir)],
                capture_output=True,
                text=True,
                check=False,
            )
            if cfg_proc.returncode == 0:
                subprocess.run(
                    [cmake, "--build", str(build_dir), "--config", "Release"],
                    check=True,
                )
                test_proc = subprocess.run(
                    [ctest, "--test-dir", str(build_dir), "-C", "Release", "--output-on-failure", "-V"],
                    capture_output=True,
                    text=True,
                    timeout=60,
                    check=False,
                )
                self.assertEqual(
                    test_proc.returncode,
                    0,
                    f"ctest test_rfb_core failed:\nSTDOUT:\n{test_proc.stdout}\nSTDERR:\n{test_proc.stderr}",
                )
                self.assertIn("CHECKS PASSED!", test_proc.stdout)
                return

        # Fallback to MinGW/GNU g++ + gcc on Windows when CMake/VS generator is unavailable
        gxx = shutil.which("g++") or shutil.which("x86_64-w64-mingw32-g++")
        gcc = shutil.which("gcc") or shutil.which("x86_64-w64-mingw32-gcc")
        if not gxx or not gcc:
            self.skipTest("Neither CMake C/C++ toolchain nor MinGW g++/gcc found on Windows host")

        miniz_obj = tmp_path / "miniz.o"
        subprocess.run(
            [gcc, "-O2", "-Wall", "-Wextra", "-c", str(CORE_DIR / "miniz.c"), "-o", str(miniz_obj)],
            check=True,
        )
        exe_path = tmp_path / "test_rfb_core_bin.exe"
        cpp_sources = [
            str(TEST_CPP),
            str(CORE_DIR / "rfb_adaptive.cpp"),
            str(CORE_DIR / "rfb_crypto.cpp"),
            str(CORE_DIR / "rfb_decoder.cpp"),
            str(CORE_DIR / "rfb_session.cpp"),
            str(CORE_DIR / "rfb_transport.cpp"),
            str(WIN_UI_DIR / "cli_options.cpp"),
            str(miniz_obj),
        ]
        fg_cpp = WIN_UI_DIR / "d3d_frame_interpolator.cpp"
        if os.path.exists(fg_cpp):
            cpp_sources.append(str(fg_cpp))
        subprocess.run(
            [
                gxx,
                "-std=c++17",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{INC_DIR}",
                f"-I{CORE_DIR}",
                f"-I{WIN_UI_DIR}",
                *cpp_sources,
                "-lws2_32",
                "-lbcrypt",
                "-ld3d11",
                "-ldxgi",
                "-ld2d1",
                "-luuid",
                "-o",
                str(exe_path),
            ],
            check=True,
        )
        proc = subprocess.run(
            [str(exe_path)],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(
            proc.returncode,
            0,
            f"test_rfb_core.exe failed:\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}",
        )
        self.assertIn("CHECKS PASSED!", proc.stdout)

    def test_native_cpp_unit_and_integration_suite(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            if os.name == "nt":
                self._run_windows_native_suite(tmp_path)
                return

            cxx = shutil.which("clang++") or shutil.which("g++") or shutil.which("c++")
            cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
            if not cxx or not cc:
                self.skipTest("No native C/C++17 compiler found")

            miniz_obj = tmp_path / "miniz.o"
            subprocess.run(
                [cc, "-O2", "-Wall", "-Wextra", "-c", str(CORE_DIR / "miniz.c"), "-o", str(miniz_obj)],
                check=True,
            )

            exe_path = tmp_path / "test_rfb_core_bin"
            cpp_sources = [
                str(TEST_CPP),
                str(CORE_DIR / "rfb_adaptive.cpp"),
                str(CORE_DIR / "rfb_crypto.cpp"),
                str(CORE_DIR / "rfb_decoder.cpp"),
                str(CORE_DIR / "rfb_session.cpp"),
                str(CORE_DIR / "rfb_transport.cpp"),
                str(WIN_UI_DIR / "cli_options.cpp"),
                str(miniz_obj),
            ]
            fg_cpp = WIN_UI_DIR / "d3d_frame_interpolator.cpp"
            if os.path.exists(fg_cpp):
                cpp_sources.append(str(fg_cpp))
            subprocess.run(
                [
                    cxx,
                    "-std=c++17",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{INC_DIR}",
                    f"-I{CORE_DIR}",
                    f"-I{WIN_UI_DIR}",
                    *cpp_sources,
                    "-pthread",
                    "-o",
                    str(exe_path),
                ],
                check=True,
            )

            proc = subprocess.run(
                [str(exe_path)],
                capture_output=True,
                text=True,
                timeout=30,
                check=False,
            )
            self.assertEqual(
                proc.returncode,
                0,
                f"test_rfb_core failed:\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}",
            )
            self.assertIn("ALL ", proc.stdout)
            self.assertIn("CHECKS PASSED!", proc.stdout)

    def test_mingw_w64_zero_dll_cross_compile(self) -> None:
        mingw_cxx = shutil.which("x86_64-w64-mingw32-g++")
        mingw_cc = shutil.which("x86_64-w64-mingw32-gcc")
        mingw_objdump = shutil.which("x86_64-w64-mingw32-objdump")
        if not mingw_cxx or not mingw_cc:
            self.skipTest("x86_64-w64-mingw32 toolchain not installed")

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            miniz_obj = tmp_path / "miniz_win.o"
            subprocess.run(
                [mingw_cc, "-O2", "-Wall", "-Wextra", "-Werror", "-c", str(CORE_DIR / "miniz.c"), "-o", str(miniz_obj)],
                check=True,
            )

            exe_path = tmp_path / "test_rfb_core.exe"
            cpp_sources = [
                str(TEST_CPP),
                str(CORE_DIR / "rfb_adaptive.cpp"),
                str(CORE_DIR / "rfb_crypto.cpp"),
                str(CORE_DIR / "rfb_decoder.cpp"),
                str(CORE_DIR / "rfb_session.cpp"),
                str(CORE_DIR / "rfb_transport.cpp"),
                str(WIN_UI_DIR / "cli_options.cpp"),
                str(miniz_obj),
            ]
            fg_cpp = WIN_UI_DIR / "d3d_frame_interpolator.cpp"
            if os.path.exists(fg_cpp):
                cpp_sources.append(str(fg_cpp))
            subprocess.run(
                [
                    mingw_cxx,
                    "-std=c++17",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{INC_DIR}",
                    f"-I{CORE_DIR}",
                    f"-I{WIN_UI_DIR}",
                    *cpp_sources,
                    "-static",
                    "-static-libgcc",
                    "-static-libstdc++",
                    "-lws2_32",
                    "-lbcrypt",
                    "-ld3d11",
                    "-ldxgi",
                    "-ld2d1",
                    "-luuid",
                    "-o",
                    str(exe_path),
                ],
                check=True,
            )

            if mingw_objdump:
                dump = subprocess.run(
                    [mingw_objdump, "-p", str(exe_path)],
                    capture_output=True,
                    text=True,
                    check=True,
                )
                dlls = [
                    line.split("DLL Name:", 1)[1].strip().lower()
                    for line in dump.stdout.splitlines()
                    if "DLL Name:" in line
                ]
                allowed_system_dlls = {
                    "kernel32.dll",
                    "msvcrt.dll",
                    "ucrtbase.dll",
                    "ws2_32.dll",
                    "bcrypt.dll",
                    "advapi32.dll",
                    "user32.dll",
                    "d3d11.dll",
                    "dxgi.dll",
                    "d2d1.dll",
                }
                for dll in dlls:
                    self.assertTrue(
                        dll in allowed_system_dlls or dll.startswith("api-ms-win-"),
                        f"Unexpected non-system DLL dependency in cross-compiled binary: {dll} (all: {dlls})",
                    )


if __name__ == "__main__":
    unittest.main()
