"""Exercise DPI tab layout and cached Client painting across window modes.

Only newly launched processes are controlled. Client receives exactly one
synthetic JPEG from a local protocol fixture, so later repaint checks cannot
be rescued by another incoming frame. Explicit --ui-paint-self-test opt-in lets
Client compose its current canvas in-process and report selected synthetic
pixel values. No cross-process HDC or saved image is used. A real Server
separately verifies capture exclusion against a synthetic background.

Example:
  python tests/ui_modes_smoke.py --bin-dir build-v1.1.0/bin/Release --expected-dpi 192
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import io
import json
import math
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time

from runtime_smoke import ADMIN_PASSWORD, PASSWORD, create_fixtures, require, wait_until, wide_field
from capture_exclusion_smoke import PhysicalUI, SyntheticWindow, capture_frame, check_region


DISPLAY_ID, CANVAS_ID = 4100, 1028
BAR_ID, TOGGLE_ID, FIT_ID, CLOSE_ID = 4101, 4102, 4103, 4104
FRAME_COLORS = ((205, 55, 130), (40, 150, 220), (230, 150, 35), (70, 200, 90))


class StaticFrameServer:
    """A deterministic protocol fixture that never refreshes its single frame."""

    def __init__(self, width=1600, height=1000):
        from PIL import Image, ImageDraw
        self.width, self.height = width, height
        frame = Image.new("RGB", (width, height))
        draw = ImageDraw.Draw(frame)
        for row in range(4):
            for column in range(6):
                draw.rectangle((column * width // 6, row * height // 4,
                                (column + 1) * width // 6 - 1, (row + 1) * height // 4 - 1),
                               fill=FRAME_COLORS[(column + row) % 4])
        stream = io.BytesIO()
        frame.save(stream, "JPEG", quality=95)
        self.jpeg = stream.getvalue()
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.stop = threading.Event()
        self.disconnected = threading.Event()
        self.images_sent = 0
        self.error = None
        self.connection = None
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    @staticmethod
    def _exact(connection, length):
        result = bytearray()
        while len(result) < length:
            part = connection.recv(length - len(result))
            if not part:
                raise EOFError("Client disconnected before authentication")
            result.extend(part)
        return bytes(result)

    def _run(self):
        try:
            while not self.stop.is_set():
                try:
                    self.connection, _ = self.listener.accept()
                    break
                except socket.timeout:
                    continue
            if self.connection is None:
                return
            with self.connection as connection:
                connection.settimeout(10)
                connection.sendall(struct.pack("<Biii", 5, 0x5100, 0x5130, 1))
                kind, name, password = struct.unpack("<B20s20s", self._exact(connection, 41))
                require(kind == 4 and name.rstrip(b"\0") == b"view" and password.rstrip(b"\0") == PASSWORD.encode("ascii"),
                        "Unexpected synthetic-fixture credentials")
                connection.sendall(struct.pack("<Biii", 5, 0x5100, 0x5110, 0))
                connection.sendall(struct.pack("<Biii", 5, 0x5200, self.width, self.height))
                connection.sendall(struct.pack("<BiiiiI", 1, 0, 0, self.width, self.height, len(self.jpeg)) + self.jpeg)
                self.images_sent = 1
                connection.settimeout(0.2)
                while not self.stop.is_set():
                    try:
                        if not connection.recv(4096):
                            break
                    except socket.timeout:
                        continue
                    # Display parameter packets are accepted but deliberately
                    # never cause another image to be sent.
        except Exception as exc:
            if not self.stop.is_set():
                self.error = str(exc)
        finally:
            self.disconnected.set()

    def close(self):
        self.stop.set()
        if self.connection:
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.listener.close()
        self.thread.join(timeout=5)
        require(not self.thread.is_alive(), "Synthetic protocol thread did not stop")


class ModeUI(PhysicalUI):
    def __init__(self):
        super().__init__()
        api = self.api
        api.GetParent.argtypes = [wintypes.HWND]
        api.GetParent.restype = wintypes.HWND
        api.GetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int]
        api.GetWindowLongPtrW.restype = ctypes.c_ssize_t
        api.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
        api.ClientToScreen.restype = wintypes.BOOL
        api.IsIconic.argtypes = [wintypes.HWND]
        api.IsIconic.restype = wintypes.BOOL
        api.IsZoomed.argtypes = [wintypes.HWND]
        api.IsZoomed.restype = wintypes.BOOL
        api.GetDpiForWindow.argtypes = [wintypes.HWND]
        api.GetDpiForWindow.restype = wintypes.UINT
        api.MonitorFromWindow.argtypes = [wintypes.HWND, wintypes.DWORD]
        api.MonitorFromWindow.restype = wintypes.HANDLE
        api.GetPropW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]
        api.GetPropW.restype = wintypes.HANDLE
        api.SetWindowDisplayAffinity.argtypes = [wintypes.HWND, wintypes.DWORD]
        api.SetWindowDisplayAffinity.restype = wintypes.BOOL
        api.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
        api.PrintWindow.restype = wintypes.BOOL
        api.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
        api.FindWindowW.restype = wintypes.HWND

    def children(self, parent, direct=False):
        result = []

        @self.callback_type
        def collect(window, _):
            if not direct or self.api.GetParent(window) == parent:
                result.append(window)
            return True

        self.api.EnumChildWindows(parent, collect, 0)
        return result

    def class_name(self, window):
        name = ctypes.create_unicode_buffer(128)
        self.api.GetClassNameW(window, name, len(name))
        return name.value

    def style(self, window):
        return self.api.GetWindowLongPtrW(window, -16) & 0xFFFFFFFF

    def screen_client_rect(self, window):
        origin = wintypes.POINT(0, 0)
        require(bool(self.api.ClientToScreen(window, ctypes.byref(origin))), "ClientToScreen failed")
        width, height = self.client_size(window)
        return origin.x, origin.y, origin.x + width, origin.y + height

    def monitor_rectangles(self, window):
        class MonitorInfo(ctypes.Structure):
            _fields_ = [("size", wintypes.DWORD), ("monitor", wintypes.RECT), ("work", wintypes.RECT), ("flags", wintypes.DWORD)]
        info = MonitorInfo()
        info.size = ctypes.sizeof(info)
        self.api.GetMonitorInfoW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MonitorInfo)]
        self.api.GetMonitorInfoW.restype = wintypes.BOOL
        monitor = self.api.MonitorFromWindow(window, 2)
        require(bool(self.api.GetMonitorInfoW(monitor, ctypes.byref(info))), "GetMonitorInfo failed")
        rect = lambda value: (value.left, value.top, value.right, value.bottom)
        return rect(info.monitor), rect(info.work)

    def paint_count(self, canvas):
        return int(self.api.GetPropW(canvas, "RCS.RenderPaintCount") or 0)

    def render_snapshot(self, canvas):
        """Use the explicitly enabled in-process DIB probe, never disable WDA."""
        width, height = self.client_size(canvas)
        require(0 < width <= 8192 and 0 < height <= 8192, "Unexpected canvas size")
        require(self.send(canvas, 0x8340) == 1, "Opt-in owner-process Render snapshot failed")
        ui = self

        class Snapshot:
            def __init__(self):
                self.width, self.height = width, height
                self.size = (width, height)

            def getpixel(self, position):
                x, y = position
                value = ui.send(canvas, 0x8341, x, y) & 0xFFFFFFFF
                require(value != 0xFFFFFFFF, "Owner-process snapshot pixel is invalid")
                return value & 255, (value >> 8) & 255, (value >> 16) & 255

            def __enter__(self):
                return self

            def __exit__(self, *_):
                require(ui.send(canvas, 0x8342) == 1, "Owner-process snapshot did not release")

        return Snapshot()


class RemoteRectangle:
    """Supply RECT data for common-control messages in our own child process."""

    def __init__(self, process):
        self.process = process
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel = self.kernel
        kernel.VirtualAllocEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
        kernel.VirtualAllocEx.restype = ctypes.c_void_p
        kernel.WriteProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                              ctypes.POINTER(ctypes.c_size_t)]
        kernel.ReadProcessMemory.argtypes = kernel.WriteProcessMemory.argtypes
        kernel.VirtualFreeEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD]
        # Retained Popen HANDLE avoids any possibility of PID reuse targeting
        # another process. No code is injected or executed through this buffer.
        self.handle = int(process._handle)
        self.address = kernel.VirtualAllocEx(self.handle, None, 16, 0x3000, 0x04)
        require(bool(self.address), "Cannot allocate a RECT in our Server process")

    def exchange(self, ui, window, message, wparam=0, rectangle=(0, 0, 0, 0)):
        require(self.process.poll() is None and ui.pid(window) == self.process.pid, "RECT target is not our live Server")
        data = ctypes.create_string_buffer(struct.pack("<iiii", *rectangle))
        length = ctypes.c_size_t()
        require(bool(self.kernel.WriteProcessMemory(self.handle, self.address, data, 16, ctypes.byref(length))) and length.value == 16,
                "WriteProcessMemory RECT failed")
        ui.send(window, message, wparam, self.address)
        require(bool(self.kernel.ReadProcessMemory(self.handle, self.address, data, 16, ctypes.byref(length))) and length.value == 16,
                "ReadProcessMemory RECT failed")
        return struct.unpack("<iiii", data.raw[:16])

    def close(self):
        if self.address:
            self.kernel.VirtualFreeEx(self.handle, self.address, 0, 0x8000)
            self.address = None


def contains(outer, inner, tolerance=2):
    return (inner[0] >= outer[0] - tolerance and inner[1] >= outer[1] - tolerance and
            inner[2] <= outer[2] + tolerance and inner[3] <= outer[3] + tolerance)


def inspect_server_tabs(ui, process, main_window, expected_dpi, timeout):
    dpi = ui.api.GetDpiForWindow(main_window)
    require(dpi == expected_dpi, f"Requested DPI scenario {expected_dpi}, actual Server DPI {dpi}")
    tab = ui.api.GetDlgItem(main_window, 1000)
    require(ui.send(tab, 0x1304) == 4, "Server does not have four tabs")
    pages = [window for window in ui.children(main_window) if ui.class_name(window) == "#32770"]
    require(len(pages) == 4, f"Expected four tab page dialogs, found {len(pages)}")
    # Resource order: service, connections, accounts, about.
    expected = [next((p for p in pages if ui.api.GetDlgItem(p, item)), None) for item in (1005, 1031, 1022)]
    expected += [next((p for p in pages if p not in expected), None)]
    require(all(expected) and len(set(expected)) == 4, "Cannot identify the four tab pages")
    main_client = ui.screen_client_rect(main_window)
    footer = [ui.rect(ui.api.GetDlgItem(main_window, item)) for item in (1007, 1027)]
    remote = RemoteRectangle(process)
    details = []
    try:
        for index, page in enumerate(expected):
            # TCM_SETCURFOCUS changes selection and issues TCN_SELCHANGE inside
            # the target process; cross-process WM_NOTIFY pointers are avoided.
            ui.send(tab, 0x1330, index)
            wait_until(lambda: ui.send(tab, 0x130B) == index and ui.api.IsWindowVisible(page), timeout,
                       f"Tab {index} selection did not activate its expected page")
            visible = [p for p in pages if ui.api.IsWindowVisible(p)]
            require(visible == [page], "Multiple Server tab pages are visible")
            tab_client = ui.screen_client_rect(tab)
            width, height = ui.client_size(tab)
            adjusted = remote.exchange(ui, tab, 0x1328, 0, (0, 0, width, height))
            content = (adjusted[0] + tab_client[0], adjusted[1] + tab_client[1],
                       adjusted[2] + tab_client[0], adjusted[3] + tab_client[1])
            headers = [remote.exchange(ui, tab, 0x130A, i) for i in range(4)]
            header_bottom = max(rect[3] for rect in headers) + tab_client[1]
            page_rect = ui.rect(page)
            require(all(abs(a - b) <= 2 for a, b in zip(page_rect, content)), "Page does not match TCM_ADJUSTRECT content area")
            require(page_rect[1] >= header_bottom, "Tab page overlaps tab headers")
            require(contains(main_client, ui.rect(tab)), "Tab control extends outside Server client area")
            require(all(button[1] >= ui.rect(tab)[3] + 1 and contains(main_client, button) for button in footer),
                    "Server footer is clipped or overlapped by tab area")
            page_client = ui.screen_client_rect(page)
            controls = [child for child in ui.children(page, direct=True) if ui.api.IsWindowVisible(child)]
            clipped = [ui.api.GetDlgCtrlID(child) for child in controls if not contains(page_client, ui.rect(child))]
            require(not clipped, f"Tab {index} controls extend outside page: {clipped}")
            details.append({"index": index, "page_rectangle": list(page_rect), "tab_header_bottom": header_bottom,
                            "visible_controls": len(controls), "clipped_controls": 0})
    finally:
        remote.close()
        ui.send(tab, 0x1330, 0)
    return {"dpi": dpi, "pages": details, "visible_pages_per_selection": 1,
            "main_client_rectangle": list(main_client), "footer_rectangles": [list(rect) for rect in footer]}


def validate_printed_frame(image, source_width, source_height):
    scale = min(image.width / source_width, image.height / source_height)
    width = max(1, math.floor(source_width * scale + 0.5))
    height = max(1, math.floor(source_height * scale + 0.5))
    left, top = (image.width - width) // 2, (image.height - height) // 2
    errors, black = [], 0
    for row in range(1, 12):
        for column in range(1, 16):
            x, y = left + width * column // 16, top + height * row // 12
            fx, fy = (x - left) * 6 / width, (y - top) * 4 / height
            if min(fx % 1, 1 - fx % 1) * width / 6 < 6 or min(fy % 1, 1 - fy % 1) * height / 4 < 6:
                continue
            expected = FRAME_COLORS[(int(fx) + int(fy)) % 4]
            actual = image.getpixel((x, y))
            black += max(actual) < 12
            errors.append(max(abs(a - b) for a, b in zip(actual, expected)))
    require(len(errors) >= 40, "Insufficient interior frame sample points")
    result = {"canvas_size": list(image.size), "samples": len(errors), "black_samples": black,
              "mismatched_samples": sum(value > 35 for value in errors), "max_rgb_error": max(errors)}
    require(result["mismatched_samples"] == 0 and black == 0, "Application paint composition differs from retained synthetic JPEG: " + json.dumps(result))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--expected-dpi", type=int, default=192)
    parser.add_argument("--keep-data", action="store_true")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This regression needs an interactive Windows desktop")
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        parser.error("Pillow is required for synthetic JPEG and DIB comparisons")
    binary_dir = args.bin_dir.resolve()
    for name in ("Client.exe", "Server.exe"):
        if not (binary_dir / name).is_file():
            parser.error(f"Missing new executable: {binary_dir / name}")
    ui = ModeUI()
    root = Path(tempfile.mkdtemp(prefix="rcs-ui-modes-")).resolve()
    processes, windows, cases = [], [], []
    fixture = None
    success = False
    deferred_failures = []
    original_cursor = ui.cursor()
    cursor_moved = False
    taskbar = ui.api.FindWindowW("Shell_TrayWnd", None)
    taskbar_was_visible = bool(taskbar and ui.api.IsWindowVisible(taskbar))
    report = {"binary_directory": str(binary_dir), "temporary_directory": str(root), "cases": cases, "screenshots_saved": False}

    def record(name, details=None, status="pass"):
        cases.append({"name": name, "status": status, "details": details or {}})
        print(json.dumps(cases[-1], ensure_ascii=True), flush=True)

    def launch(name, data):
        command = [str(binary_dir / name), "--data-dir", str(data)]
        if name == "Client.exe":
            command.append("--ui-paint-self-test")
        process = subprocess.Popen(command, cwd=data)
        processes.append(process)
        return process

    try:
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        server_data, client_data = create_fixtures(root, port)
        with (server_data / "user.plain").open("ab") as users:
            users.write(struct.pack("<i40s40s", 0, wide_field("observer0", 20), wide_field(PASSWORD, 20)))
        server = launch("Server.exe", server_data)
        auth = ui.dialog(server, (1017, 1), args.timeout)
        ui.set_text(ui.api.GetDlgItem(auth, 1017), ADMIN_PASSWORD)
        ui.click(auth, 1)
        server_window = ui.dialog(server, (1000, 1007), args.timeout)
        record("server_tabs_dpi_layout", inspect_server_tabs(ui, server, server_window, args.expected_dpi, args.timeout))
        ui.click(server_window, 1007)

        background = SyntheticWindow(ui, (0, 0, ui.width, ui.height))
        windows.append(background)
        fixture = StaticFrameServer()
        config = client_data / "ClientConfig.xml"
        config.write_text(config.read_text(encoding="ascii").replace(f'Port="{port}"', f'Port="{fixture.port}"'), encoding="ascii")
        client = launch("Client.exe", client_data)
        login = ui.dialog(client, (1000, 1012, 1003, 1004), args.timeout)
        ui.raise_own(client, login)
        require(ui.send(ui.api.GetDlgItem(login, 1000), 0x014E, 1) != ctypes.c_size_t(-1).value, "Missing synthetic host")
        ui.set_text(ui.api.GetDlgItem(login, 1012), "view")
        ui.set_text(ui.api.GetDlgItem(login, 1003), PASSWORD)
        ui.click(login, 1004)
        main_window = ui.dialog(client, (DISPLAY_ID, BAR_ID), args.timeout, expected_title="Remote Control Client")
        ui.place_own(client, main_window, ui.width // 8, ui.height // 8, ui.width * 3 // 5, ui.height * 3 // 5)
        display = ui.api.GetDlgItem(main_window, DISPLAY_ID)
        canvas = ui.api.GetDlgItem(display, CANVAS_ID)
        bar = ui.api.GetDlgItem(main_window, BAR_ID)
        require(canvas and ui.api.GetParent(display) == main_window and ui.api.GetParent(bar) == main_window,
                "Display and control bar must be children of the Client main window")
        require(all(ui.api.GetDlgItem(bar, item) for item in (TOGGLE_ID, FIT_ID, CLOSE_ID)), "Floating bar controls are incomplete")

        def verify_paint(name):
            latest = {}
            def painted():
                require(client.poll() is None and fixture.error is None, "Client or synthetic server failed")
                try:
                    with ui.render_snapshot(canvas) as image:
                        value = validate_printed_frame(image, fixture.width, fixture.height)
                    latest.clear()
                    latest.update(value)
                    return latest
                except AssertionError as exc:
                    latest["last_error"] = str(exc)
                    return None
            try:
                evidence = wait_until(painted, args.timeout, "Application-side cached frame repaint failed")
            except TimeoutError as exc:
                raise AssertionError(f"{exc}: {latest}") from exc
            require(fixture.images_sent == 1, "Fixture unexpectedly refreshed the frame")
            count = wait_until(lambda: ui.paint_count(canvas), args.timeout, "Canvas did not report a successful real WM_PAINT")
            record(name, {**evidence, "successful_wm_paint_count": count, "fixture_images_sent": fixture.images_sent})

        ui.click(bar, FIT_ID)
        verify_paint("initial_cached_frame")
        original_rect, original_style = ui.rect(main_window), ui.style(main_window)
        require(original_style & 0x00C00000, "Window mode has no caption")
        monitor, work = ui.monitor_rectangles(main_window)

        def enter_fullscreen():
            nonlocal cursor_moved
            ui.click(bar, TOGGLE_ID)
            wait_until(lambda: ui.rect(main_window) == monitor and not (ui.style(main_window) & 0x01C40000), args.timeout,
                       "Fullscreen did not cover rcMonitor without caption/frame")
            try:
                wait_until(lambda: ui.rect(display) == monitor and ui.screen_client_rect(main_window) == monitor,
                           args.timeout, "Fullscreen client area and Display did not fill rcMonitor")
            except TimeoutError as exc:
                raise AssertionError(f"{exc}; main={ui.rect(main_window)}, client={ui.screen_client_rect(main_window)}, "
                                     f"display={ui.rect(display)}, monitor={monitor}, "
                                     f"exstyle={ui.api.GetWindowLongPtrW(main_window, -20):#x}") from exc
            require(not (ui.api.GetWindowLongPtrW(main_window, -20) & 0x20301), "Fullscreen retained extended frame-edge styles")
            # Hover only our own full-screen grab strip; no mouse buttons or
            # global keyboard events are synthesized. Restore the pointer later.
            ui.api.SetCursorPos((monitor[0] + monitor[2]) // 2, monitor[1] + 3)
            cursor_moved = True
            wait_until(lambda: ui.rect(bar)[1] == monitor[1], args.timeout, "Floating bar did not expand on hover")
            bar_rect = ui.rect(bar)
            require(contains(monitor, bar_rect) and bar_rect[1] <= monitor[1] + args.expected_dpi,
                    "Floating bar is outside the top of the monitor")
            require(ui.api.IsWindowVisible(bar), "Floating control bar is not visible")
            require(all(ui.api.IsWindowVisible(ui.api.GetDlgItem(bar, item)) and contains(bar_rect, ui.rect(ui.api.GetDlgItem(bar, item)))
                        for item in (TOGGLE_ID, FIT_ID, CLOSE_ID)), "Expanded bar buttons are clipped")
            if taskbar:
                require(bool(ui.api.IsWindowVisible(taskbar)) == taskbar_was_visible, "Fullscreen changed the system taskbar visibility flag")
            ui.click(bar, FIT_ID)
            return bar_rect

        bar_rect = enter_fullscreen()
        verify_paint("fullscreen_cached_frame")
        record("fullscreen_monitor_geometry", {"window": list(ui.rect(main_window)), "monitor": list(monitor),
                                                 "client_area": list(ui.screen_client_rect(main_window)),
                                                 "display_rectangle": list(ui.rect(display)),
                                                 "work_area": list(work), "floating_bar": list(bar_rect),
                                                 "caption_present": False, "covers_taskbar_area": monitor != work})
        # The real Server sees the independent synthetic background through
        # the excluded fullscreen Client, not the fixture's different frame.
        # Fullscreen transitions can reorder topmost windows. Put our own
        # artificial backdrop immediately behind Client before checking pixels.
        require(bool(ui.api.SetWindowPos(background.window, main_window, 0, 0, 0, 0, 0x0013)), "Cannot position synthetic backdrop below Client")
        ui.flush()
        frame, tiles = capture_frame(port, "observer0", args.timeout)
        try:
            record("fullscreen_client_capture_excluded", {"desktop": check_region(frame, monitor, ui, 0),
                                                            "control_bar": check_region(frame, bar_rect, ui, 0),
                                                            "jpeg_tiles": tiles, "affinity": ui.affinity(main_window)})
        except AssertionError as exc:
            deferred_failures.append(str(exc))
            record("fullscreen_client_capture_excluded", {"error": str(exc), "affinity": ui.affinity(main_window),
                                                            "backdrop_affinity": ui.affinity(background.window),
                                                            "server_rectangle": list(ui.rect(server_window))}, "fail")

        ui.api.SetCursorPos(monitor[0] + 30, (monitor[1] + monitor[3]) // 2)
        wait_until(lambda: ui.rect(bar)[1] < monitor[1], args.timeout, "Floating bar did not auto-hide")
        collapsed = ui.rect(bar)
        require(0 < collapsed[3] - monitor[1] <= args.expected_dpi // 4, "Auto-hide did not leave a small visible grab strip")
        record("fullscreen_bar_auto_hide", {"bar_rectangle": list(collapsed), "visible_grab_height": collapsed[3] - monitor[1]})
        ui.api.SetCursorPos((monitor[0] + monitor[2]) // 2, monitor[1] + 3)
        wait_until(lambda: ui.rect(bar)[1] == monitor[1], args.timeout, "Floating bar could not be expanded again")
        record("fullscreen_bar_hover_restore", {"bar_rectangle": list(ui.rect(bar))})

        ui.api.PostMessageW(canvas, 0x0100, 27, 1)  # Targeted ESC, not global SendInput.
        ui.api.PostMessageW(canvas, 0x0101, 27, 0xC0000001)
        wait_until(lambda: ui.rect(main_window) == original_rect and (ui.style(main_window) & 0x00C00000), args.timeout,
                   "ESC did not restore window mode and placement")
        verify_paint("escape_restores_cached_frame")
        record("escape_restores_window", {"window": list(ui.rect(main_window)), "caption_restored": True})

        for round_index in range(3):
            before = ui.paint_count(canvas)
            ui.api.PostMessageW(main_window, 0x0112, 0xF020, 0)  # SC_MINIMIZE
            wait_until(lambda: ui.api.IsIconic(main_window), args.timeout, "Client did not minimize")
            time.sleep(0.2)
            ui.api.PostMessageW(main_window, 0x0112, 0xF120, 0)  # SC_RESTORE
            wait_until(lambda: not ui.api.IsIconic(main_window), args.timeout, "Client did not restore")
            wait_until(lambda: ui.paint_count(canvas) > before, args.timeout, "Restore did not invoke a successful WM_PAINT")
            ui.click(bar, FIT_ID)
            verify_paint(f"minimize_restore_cached_frame_{round_index + 1}")
            require(ui.rect(main_window) == original_rect, "Minimize/restore changed the saved window placement")

        # The server fixture is silent, so covering/uncovering cannot refresh
        # the remote frame even though it invalidates the native window surface.
        cover = SyntheticWindow(ui, ui.rect(main_window), solid=(245, 245, 245))
        windows.append(cover)
        time.sleep(0.2)
        before = ui.paint_count(canvas)
        cover.close()
        ui.api.InvalidateRect(canvas, None, False)
        ui.api.UpdateWindow(canvas)
        wait_until(lambda: ui.paint_count(canvas) > before, args.timeout, "Uncover did not run WM_PAINT")
        verify_paint("occlusion_repaint_retains_frame")

        enter_fullscreen()
        ui.click(bar, TOGGLE_ID)
        wait_until(lambda: ui.rect(main_window) == original_rect and (ui.style(main_window) & 0x00C00000), args.timeout,
                   "Floating exit-fullscreen control did not restore the window")
        verify_paint("toolbar_exit_restores_frame")
        record("toolbar_exit_fullscreen", {"window": list(ui.rect(main_window))})

        ui.api.PostMessageW(main_window, 0x0112, 0xF030, 0)  # SC_MAXIMIZE
        wait_until(lambda: ui.api.IsZoomed(main_window), args.timeout, "Client did not maximize")
        maximized_rect = ui.rect(main_window)
        enter_fullscreen()
        verify_paint("maximized_to_fullscreen_cached_frame")
        require(not ui.api.IsZoomed(main_window), "Fullscreen retained WS_MAXIMIZE state")
        before = ui.paint_count(canvas)
        ui.api.PostMessageW(main_window, 0x0112, 0xF020, 0)
        wait_until(lambda: ui.api.IsIconic(main_window), args.timeout, "Fullscreen Client did not minimize")
        time.sleep(0.2)
        ui.api.PostMessageW(main_window, 0x0112, 0xF120, 0)
        wait_until(lambda: not ui.api.IsIconic(main_window) and ui.rect(main_window) == monitor and
                   not (ui.style(main_window) & 0x01C40000), args.timeout,
                   "Taskbar-equivalent restore did not retain full monitor mode")
        wait_until(lambda: ui.paint_count(canvas) > before, args.timeout, "Fullscreen restore did not paint")
        ui.click(bar, FIT_ID)
        verify_paint("fullscreen_minimize_restore_cached_frame")
        ui.click(bar, TOGGLE_ID)
        wait_until(lambda: ui.api.IsZoomed(main_window) and ui.rect(main_window) == maximized_rect and
                   (ui.style(main_window) & 0x00C00000), args.timeout,
                   "Exiting fullscreen did not restore the previous maximized state")
        verify_paint("fullscreen_exit_restores_maximized_frame")
        record("maximized_fullscreen_roundtrip", {"maximized_rectangle": list(maximized_rect),
                                                   "restored_rectangle": list(ui.rect(main_window)),
                                                   "is_maximized": True})
        ui.api.PostMessageW(main_window, 0x0112, 0xF120, 0)
        wait_until(lambda: not ui.api.IsZoomed(main_window) and ui.rect(main_window) == original_rect, args.timeout,
                   "Returning from maximized mode lost the original normal placement")
        ui.click(bar, CLOSE_ID)
        # MFC may return the last WM_COMMAND value for this explicit button
        # path, just as its dialog paths can return IDOK/IDCANCEL.
        client_exit = client.wait(timeout=args.timeout)
        require(client_exit in (0, 2, CLOSE_ID), f"Floating disconnect/close returned unexpected exit code {client_exit}")
        require(fixture.disconnected.wait(3), "Client connection remained open after closing")
        ui.click(server_window, 1007)
        time.sleep(0.2)
        ui.api.PostMessageW(server_window, 0x0010, 0, 0)
        require(server.wait(timeout=args.timeout) in (0, 1, 2), "Server did not close normally")
        record("normal_cleanup", {"client_exit": client.returncode, "server_exit": server.returncode,
                                   "fixture_images_sent": fixture.images_sent})
        success = not deferred_failures
    except Exception as exc:
        record("ui_modes", {"exception": type(exc).__name__, "error": str(exc)}, "fail")
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                ui.close_windows(process)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=3)
                    record("forced_cleanup", {"own_pid": process.pid}, "cleanup")
        if fixture:
            fixture.close()
        for window in reversed(windows):
            window.close()
        if cursor_moved:
            ui.api.SetCursorPos(*original_cursor)
        report["success"] = success
        report["own_processes"] = [{"pid": p.pid, "exit_code": p.returncode} for p in processes]
        report["data_retained"] = args.keep_data or not success
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
        if success and not args.keep_data:
            require(root.parent == Path(tempfile.gettempdir()).resolve() and root.name.startswith("rcs-ui-modes-"),
                    "Unsafe temporary data cleanup path")
            shutil.rmtree(root)
        print(json.dumps(report, ensure_ascii=True), flush=True)
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
