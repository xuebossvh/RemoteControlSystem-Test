"""Verify Client capture exclusion against synthetic pixels received from Server.

Requires Windows 10 2004+ and Pillow. Run after building the new executables:
  python tests/capture_exclusion_smoke.py --bin-dir build-v1.0.1/bin/Release

Only processes started by this invocation are controlled. A temporary topmost
synthetic background covers the primary desktop during the test. JPEG frames
are decoded in memory and never saved; reports contain only measurements.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import io
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import uuid

from runtime_smoke import ADMIN_PASSWORD, PASSWORD, IMAGE, PIXEL, SETUP, SERVER_IDENTITY, Peer, WinUI
from runtime_smoke import create_fixtures, require, wait_until, wide_field


PALETTES = (
    ((30, 175, 65), (215, 45, 45), (40, 85, 210), (225, 185, 40)),
    ((195, 40, 180), (30, 175, 200), (235, 115, 35), (65, 60, 205)),
    ((45, 120, 205), (210, 65, 145), (65, 195, 100), (230, 165, 45)),
    ((175, 45, 215), (35, 190, 155), (220, 90, 40), (70, 115, 225)),
)
NEGATIVE_COLOR = (245, 245, 245)


class PhysicalUI(WinUI):
    def __init__(self):
        # Only this Python process changes awareness. Product processes keep
        # their own DPI policy; their image coordinates are mapped separately.
        user = ctypes.WinDLL("user32", use_last_error=True)
        user.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        user.SetProcessDpiAwarenessContext.restype = wintypes.BOOL
        aware = bool(user.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)))
        if not aware:
            user.GetThreadDpiAwarenessContext.restype = ctypes.c_void_p
            user.GetAwarenessFromDpiAwarenessContext.argtypes = [ctypes.c_void_p]
            user.GetAwarenessFromDpiAwarenessContext.restype = ctypes.c_int
            require(user.GetAwarenessFromDpiAwarenessContext(user.GetThreadDpiAwarenessContext()) == 2,
                    "The test must run in a per-monitor DPI-aware process")
        super().__init__()
        self.api.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        self.api.GetWindowRect.restype = wintypes.BOOL
        self.api.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int,
                                          ctypes.c_int, ctypes.c_int, wintypes.UINT]
        self.api.SetWindowPos.restype = wintypes.BOOL
        self.api.GetSystemMetrics.argtypes = [ctypes.c_int]
        self.api.GetSystemMetrics.restype = ctypes.c_int
        self.api.GetWindowDisplayAffinity.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        self.api.GetWindowDisplayAffinity.restype = wintypes.BOOL
        self.api.UpdateWindow.argtypes = [wintypes.HWND]
        self.api.InvalidateRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT), wintypes.BOOL]
        self.width, self.height = self.api.GetSystemMetrics(0), self.api.GetSystemMetrics(1)
        require(self.width >= 800 and self.height >= 600, "The test needs a primary desktop of at least 800x600")
        self.dwm = ctypes.WinDLL("dwmapi")
        self.dwm.DwmFlush.restype = ctypes.c_long

    def rect(self, window: int) -> tuple[int, int, int, int]:
        value = wintypes.RECT()
        require(bool(self.api.GetWindowRect(window, ctypes.byref(value))), "GetWindowRect failed")
        return value.left, value.top, value.right, value.bottom

    def place_own(self, process: subprocess.Popen, window: int, x: int, y: int,
                  width: int | None = None, height: int | None = None) -> None:
        require(self.pid(window) == process.pid and process.poll() is None, "Window is not our live process")
        flags = 0x0010  # SWP_NOACTIVATE; no global mouse or keyboard input.
        if width is None:
            flags |= 0x0001  # SWP_NOSIZE
        require(bool(self.api.SetWindowPos(window, ctypes.c_void_p(-1), x, y, width or 0, height or 0, flags)),
                "Could not place our test window above the synthetic background")
        self.flush()

    def raise_own(self, process: subprocess.Popen, window: int) -> None:
        left, top, _, _ = self.rect(window)
        self.place_own(process, window, left, top)

    def flush(self):
        self.dwm.DwmFlush()
        time.sleep(0.12)

    def affinity(self, window: int):
        value = wintypes.DWORD()
        return value.value if self.api.GetWindowDisplayAffinity(window, ctypes.byref(value)) else None


class SyntheticWindow:
    """An owned, non-excluded Win32 window with its own message-pump thread."""

    def __init__(self, ui: PhysicalUI, rect: tuple[int, int, int, int], solid=None):
        self.ui, self.bounds, self.solid = ui, rect, solid
        self.phase = 0
        self.window = None
        self.error = None
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        require(self.ready.wait(5), "Synthetic window did not initialize")
        if self.error:
            raise self.error
        require(bool(self.window), "Synthetic window creation failed")
        ui.flush()

    def _run(self):
        # Keep each thread's local structure/function bindings separate.
        api = ctypes.WinDLL("user32", use_last_error=True)
        gdi = ctypes.WinDLL("gdi32", use_last_error=True)
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
        kernel.GetModuleHandleW.restype = wintypes.HMODULE
        proc_type = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t)

        class WindowClass(ctypes.Structure):
            _fields_ = [("style", wintypes.UINT), ("procedure", proc_type), ("class_extra", ctypes.c_int),
                        ("window_extra", ctypes.c_int), ("instance", wintypes.HINSTANCE), ("icon", wintypes.HICON),
                        ("cursor", wintypes.HANDLE), ("background", wintypes.HBRUSH),
                        ("menu_name", wintypes.LPCWSTR), ("class_name", wintypes.LPCWSTR)]

        class PaintState(ctypes.Structure):
            _fields_ = [("dc", wintypes.HDC), ("erase", wintypes.BOOL), ("rectangle", wintypes.RECT),
                        ("restore", wintypes.BOOL), ("incremental", wintypes.BOOL), ("reserved", ctypes.c_byte * 32)]

        api.RegisterClassW.argtypes = [ctypes.POINTER(WindowClass)]
        api.RegisterClassW.restype = wintypes.ATOM
        api.UnregisterClassW.argtypes = [wintypes.LPCWSTR, wintypes.HINSTANCE]
        api.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
                                        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                        wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, ctypes.c_void_p]
        api.CreateWindowExW.restype = wintypes.HWND
        api.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t]
        api.DefWindowProcW.restype = ctypes.c_ssize_t
        api.BeginPaint.argtypes = [wintypes.HWND, ctypes.POINTER(PaintState)]
        api.BeginPaint.restype = wintypes.HDC
        api.EndPaint.argtypes = [wintypes.HWND, ctypes.POINTER(PaintState)]
        api.FillRect.argtypes = [wintypes.HDC, ctypes.POINTER(wintypes.RECT), wintypes.HBRUSH]
        api.DestroyWindow.argtypes = [wintypes.HWND]
        api.PostQuitMessage.argtypes = [ctypes.c_int]
        api.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT]
        api.GetMessageW.restype = ctypes.c_int
        api.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
        api.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
        api.DispatchMessageW.restype = ctypes.c_ssize_t
        api.UpdateWindow.argtypes = [wintypes.HWND]
        api.InvalidateRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT), wintypes.BOOL]
        gdi.CreateSolidBrush.argtypes = [wintypes.DWORD]
        gdi.CreateSolidBrush.restype = wintypes.HBRUSH
        gdi.DeleteObject.argtypes = [wintypes.HANDLE]

        def fill(dc, rectangle, color):
            red, green, blue = color
            brush = gdi.CreateSolidBrush(red | green << 8 | blue << 16)
            require(bool(brush), "CreateSolidBrush failed")
            try:
                api.FillRect(dc, ctypes.byref(rectangle), brush)
            finally:
                gdi.DeleteObject(brush)

        @proc_type
        def procedure(window, message, wparam, lparam):
            try:
                if message == 0x000F:  # WM_PAINT
                    paint = PaintState()
                    dc = api.BeginPaint(window, ctypes.byref(paint))
                    try:
                        width, height = self.ui.client_size(window)
                        if self.solid is not None:
                            fill(dc, wintypes.RECT(0, 0, width, height), self.solid)
                        else:
                            palette = PALETTES[self.phase % len(PALETTES)]
                            for row in range(6):
                                for column in range(8):
                                    fill(dc, wintypes.RECT(column * width // 8, row * height // 6,
                                                          (column + 1) * width // 8, (row + 1) * height // 6),
                                         palette[(row + column) % 4])
                    finally:
                        api.EndPaint(window, ctypes.byref(paint))
                    return 0
                if message == 0x0010:
                    api.DestroyWindow(window)
                    return 0
                if message == 0x0002:
                    api.PostQuitMessage(0)
                    return 0
            except Exception as exc:
                self.error = exc
            return api.DefWindowProcW(window, message, wparam, lparam)

        name = "RcsSyntheticCaptureTest-" + uuid.uuid4().hex
        instance = kernel.GetModuleHandleW(None)
        klass = WindowClass(0, procedure, 0, 0, instance, None, None, None, None, name)
        try:
            require(bool(api.RegisterClassW(ctypes.byref(klass))), "RegisterClass failed")
            x, y, right, bottom = self.bounds
            # TOPMOST | TOOLWINDOW | NOACTIVATE, and POPUP | VISIBLE.
            self.window = api.CreateWindowExW(0x08000088, name, "Synthetic capture test", 0x90000000,
                                              x, y, right - x, bottom - y, None, None, instance, None)
            require(bool(self.window), "CreateWindowEx failed")
            api.UpdateWindow(self.window)
            self.ready.set()
            message = wintypes.MSG()
            while api.GetMessageW(ctypes.byref(message), None, 0, 0) > 0:
                api.TranslateMessage(ctypes.byref(message))
                api.DispatchMessageW(ctypes.byref(message))
        except Exception as exc:
            self.error = exc
            self.ready.set()
        finally:
            api.UnregisterClassW(name, instance)

    def set_phase(self, phase: int):
        self.phase = phase
        self.ui.api.InvalidateRect(self.window, None, False)
        self.ui.api.UpdateWindow(self.window)
        self.ui.flush()
        if self.error:
            raise self.error

    def close(self):
        if self.window and self.thread.is_alive():
            require(self.ui.pid(self.window) == os.getpid(), "Synthetic cleanup target is not this process")
            self.ui.api.PostMessageW(self.window, 0x0010, 0, 0)
            self.thread.join(timeout=5)
            require(not self.thread.is_alive(), "Synthetic window did not close")


def capture_frame(port: int, username: str, timeout: float):
    from PIL import Image

    canvas = None
    covered = set()
    area = 0
    deadline = time.monotonic() + timeout
    with Peer(port, timeout) as peer:
        require(peer.login(username, fragmented=True) == 0, "Observer did not receive viewing permission")
        while time.monotonic() < deadline:
            kind, payload = peer.message(timeout=max(0.01, deadline - time.monotonic()))
            if kind == SETUP:
                if payload[0] == SERVER_IDENTITY:
                    require(payload[1] > 0 and payload[2] > 0, "Invalid Server identity metadata")
                    continue
                require(payload[0] == PIXEL, "Unexpected observer setup")
                width, height = payload[1:]
                require(0 < width <= 8192 and 0 < height <= 8192, "Unexpected image dimensions")
                if canvas is None or canvas.size != (width, height):
                    canvas = Image.new("RGB", (width, height))
                    covered.clear()
                    area = 0
            else:
                require(kind == IMAGE and canvas is not None, "JPEG arrived before dimensions")
                rectangle, jpeg = payload
                left, top, right, bottom = rectangle
                require(0 <= left < right <= canvas.width and 0 <= top < bottom <= canvas.height, "Invalid tile rectangle")
                with Image.open(io.BytesIO(jpeg)) as tile:
                    require(tile.size == (right - left, bottom - top), "JPEG dimensions differ from wire rectangle")
                    canvas.paste(tile.convert("RGB"), (left, top))
                if rectangle not in covered:
                    covered.add(rectangle)
                    area += (right - left) * (bottom - top)
                if area == canvas.width * canvas.height:
                    return canvas, len(covered)
    raise TimeoutError("Did not receive a complete initial desktop frame")


def check_region(frame, physical_rect, ui: PhysicalUI, phase: int, solid=None) -> dict:
    left, top, right, bottom = physical_rect
    require(0 <= left < right <= ui.width and 0 <= top < bottom <= ui.height, "Test window lies outside primary desktop")
    # Map physical window coordinates to the product's screenshot coordinates.
    sx, sy = frame.width / ui.width, frame.height / ui.height
    bounds = (int(left * sx), int(top * sy), int(right * sx), int(bottom * sy))
    lx, ty, rx, by = bounds
    errors = []
    bad_points = []
    bad_gray = bad_black = 0
    palette = PALETTES[phase % len(PALETTES)]
    for row in range(1, 8):
        for column in range(1, 12):
            x, y = lx + (rx - lx) * column // 12, ty + (by - ty) * row // 8
            # Stay away from marker cell edges and JPEG block transitions.
            fx, fy = x * 8 / frame.width, y * 6 / frame.height
            if solid is None and (min(fx % 1, 1 - fx % 1) * frame.width / 8 < 7 or
                                  min(fy % 1, 1 - fy % 1) * frame.height / 6 < 7):
                continue
            expected = solid if solid is not None else palette[(int(fx) + int(fy)) % 4]
            actual = frame.getpixel((x, y))
            error = max(abs(a - b) for a, b in zip(actual, expected))
            errors.append(error)
            if error > 35:
                bad_points.append((x, y))
                bad_gray += max(actual) - min(actual) < 12
                bad_black += max(actual) < 12
    require(len(errors) >= 12, "Not enough interior sample points")
    bad = sum(error > 35 for error in errors)
    measurements = {"physical_rectangle": list(physical_rect), "protocol_rectangle": list(bounds),
                    "samples": len(errors), "mismatched_samples": bad, "max_rgb_error": max(errors),
                    "mismatched_sample_bounds": [min(p[0] for p in bad_points), min(p[1] for p in bad_points),
                                                 max(p[0] for p in bad_points), max(p[1] for p in bad_points)] if bad_points else None,
                    "mismatched_gray_samples": bad_gray, "mismatched_black_samples": bad_black}
    require(bad == 0, "Captured pixels differ from synthetic background/negative control: " + json.dumps(measurements))
    return measurements


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--keep-data", action="store_true")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("An interactive Windows desktop is required")
    try:
        from PIL import Image  # noqa: F401; explicit optional test dependency
    except ImportError:
        parser.error("Pillow is required to verify decoded wire pixels; install Pillow before running this test")
    binary_dir = args.bin_dir.resolve()
    for name in ("Client.exe", "Server.exe"):
        if not (binary_dir / name).is_file():
            parser.error(f"Missing new executable: {binary_dir / name}")
    if not 1 <= args.timeout <= 120:
        parser.error("--timeout must be in [1, 120]")
    ui = PhysicalUI()
    root = Path(tempfile.mkdtemp(prefix="rcs-capture-exclusion-")).resolve()
    processes = []
    synthetic = []
    cases = []
    success = False
    report = {"binary_directory": str(binary_dir), "physical_desktop": [ui.width, ui.height],
              "temporary_directory": str(root), "cases": cases, "screenshots_saved": False}

    def record(name, details=None, status="pass"):
        cases.append({"name": name, "status": status, "details": details or {}})
        print(json.dumps(cases[-1], ensure_ascii=True), flush=True)

    def launch(name, data):
        process = subprocess.Popen([str(binary_dir / name), "--data-dir", str(data)], cwd=data)
        processes.append(process)
        return process

    observer_index = 0

    def frame_now():
        nonlocal observer_index
        username = f"observer{observer_index}"
        observer_index += 1
        return capture_frame(port, username, args.timeout)

    try:
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        server_data, client_data = create_fixtures(root, port)
        with (server_data / "user.plain").open("ab") as users:
            for index in range(8):
                users.write(struct.pack("<i40s40s", 0, wide_field(f"observer{index}", 20), wide_field(PASSWORD, 20)))
        server = launch("Server.exe", server_data)
        auth = ui.dialog(server, (1017, 1), args.timeout)
        ui.set_text(ui.api.GetDlgItem(auth, 1017), ADMIN_PASSWORD)
        ui.click(auth, 1)
        server_window = ui.dialog(server, (1000, 1007), args.timeout)
        ui.click(server_window, 1007)

        def listening():
            require(server.poll() is None, "Our Server exited")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return True
            except OSError:
                return False

        wait_until(listening, args.timeout, "Our Server did not listen")
        background = SyntheticWindow(ui, (0, 0, ui.width, ui.height))
        synthetic.append(background)
        require(ui.affinity(background.window) == 0, "Synthetic background must not be capture-excluded")

        negative_rect = (ui.width // 3, ui.height // 3, ui.width * 2 // 3, ui.height * 2 // 3)
        negative = SyntheticWindow(ui, negative_rect, solid=NEGATIVE_COLOR)
        synthetic.append(negative)
        frame, tiles = frame_now()
        # The marker palette never has all channels near white. Locate the
        # independent white overlay before assuming a physical/image mapping;
        # a clipped bounding box exposes a product DPI/capture-area defect.
        from PIL import ImageChops
        red, green, blue = frame.split()
        minimum = ImageChops.darker(red, ImageChops.darker(green, blue))
        white_bounds = minimum.point(lambda value: 255 if value >= 225 else 0).getbbox()
        record("negative_control_geometry", {"physical_overlay": list(negative_rect),
                                               "observed_protocol_white_bounds": list(white_bounds) if white_bounds else None,
                                               "physical_desktop": [ui.width, ui.height],
                                               "protocol_desktop": list(frame.size)})
        record("non_excluded_overlay_is_captured", {**check_region(frame, negative_rect, ui, 0, NEGATIVE_COLOR),
                                                     "protocol_desktop": list(frame.size), "jpeg_tiles": tiles})
        negative.close()

        background.set_phase(1)
        frame, _ = frame_now()
        record("live_synthetic_background", check_region(frame, (0, 0, ui.width, ui.height), ui, 1))

        client = launch("Client.exe", client_data)
        login = ui.dialog(client, (1000, 1012, 1003, 1004), args.timeout)
        ui.place_own(client, login, ui.width // 8, ui.height // 8)
        frame, _ = frame_now()
        record("login_window_excluded", {**check_region(frame, ui.rect(login), ui, 1), "affinity": ui.affinity(login)})

        combo = ui.api.GetDlgItem(login, 1000)
        require(ui.send(combo, 0x014E, 1) != ctypes.c_size_t(-1).value, "Missing loopback host")
        ui.set_text(ui.api.GetDlgItem(login, 1012), "view")
        ui.set_text(ui.api.GetDlgItem(login, 1003), PASSWORD)
        ui.click(login, 1004)
        main_window = ui.dialog(client, (), args.timeout, expected_title="Remote Control Client")
        ui.place_own(client, main_window, ui.width // 10, ui.height // 8, ui.width * 3 // 5, ui.height * 3 // 5)

        def find_display():
            child = ui.api.GetDlgItem(main_window, 4100)
            if child and ui.api.GetDlgItem(child, 1028):
                return child
            return next((window for window in ui.windows(client.pid)
                         if not ui.title(window) and ui.api.GetDlgItem(window, 1028)), None)

        display = wait_until(find_display, args.timeout, "Our Client DisplayDlg was not visible")
        display_root = ui.api.GetAncestor(display, 2)
        if display_root == display:  # Legacy popup; v1.1+ is an embedded child.
            ui.raise_own(client, display)
        background.set_phase(2)
        frame, _ = frame_now()
        left, top, right, bottom = ui.rect(main_window)
        shell = (left + 6, top + 6, right - 6, min(bottom - 6, top + max(40, ui.height // 14)))
        record("main_window_excluded", {**check_region(frame, shell, ui, 2), "affinity": ui.affinity(main_window)})
        record("display_window_excluded", {**check_region(frame, ui.rect(display), ui, 2), "affinity": ui.affinity(display_root),
                                            "inherits_root_affinity": display_root != display})

        require(bool(ui.api.PostMessageW(main_window, 0x0111, 32813, 0)), "Could not open our Client settings")
        settings = ui.dialog(client, (1016, 1017, 1018), args.timeout)
        ui.place_own(client, settings, ui.width // 3, ui.height // 4)
        background.set_phase(3)
        frame, _ = frame_now()
        record("settings_window_excluded", {**check_region(frame, ui.rect(settings), ui, 3), "affinity": ui.affinity(settings)})
        record("background_changes_remain_visible", check_region(frame, (0, 0, ui.width, ui.height), ui, 3))

        ui.click(settings, 1018)
        ui.api.PostMessageW(main_window, 0x0010, 0, 0)
        require(client.wait(timeout=args.timeout) in (0, 2), "Our Client did not exit normally")
        ui.click(server_window, 1007)
        time.sleep(0.2)
        ui.api.PostMessageW(server_window, 0x0010, 0, 0)
        require(server.wait(timeout=args.timeout) in (0, 1, 2), "Our Server did not exit normally")
        success = True
    except Exception as exc:
        record("capture_exclusion", {"exception": type(exc).__name__, "error": str(exc)}, "fail")
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                ui.close_windows(process)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.terminate()  # Own Popen handle only, never by name.
                    process.wait(timeout=3)
                    record("forced_cleanup", {"own_pid": process.pid}, "cleanup")
        for window in reversed(synthetic):
            window.close()
        report["success"] = success
        report["own_processes"] = [{"pid": process.pid, "exit_code": process.returncode} for process in processes]
        report["data_retained"] = args.keep_data or not success
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
        if success and not args.keep_data:
            require(root.parent == Path(tempfile.gettempdir()).resolve() and root.name.startswith("rcs-capture-exclusion-"),
                    "Unsafe temporary cleanup path")
            shutil.rmtree(root)
        print(json.dumps(report, ensure_ascii=True), flush=True)
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
