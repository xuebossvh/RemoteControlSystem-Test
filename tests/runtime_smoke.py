"""Windows GUI and loopback protocol smoke test using only the Python stdlib.

Run explicitly on an interactive Windows desktop after building both programs:
    python tests/runtime_smoke.py --bin-dir build/bin/Release

The test creates isolated --data-dir directories, opens its own Server/Client
windows, and cleans up only processes it started. JPEG payloads stay in memory;
no screenshots are written or uploaded. It never sends a keyboard input command.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time


SETUP, IMAGE, USER, MOUSE = 5, 1, 4, 2
PERMISSION, CONTROL, IP, PIXEL = 0x5100, 0x5110, 0x5130, 0x5200
ADMIN_PASSWORD = "111"
PASSWORD = "SmokePass123"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def wait_until(predicate, timeout: float, description: str):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise TimeoutError(description)


class WinUI:
    """Use HWNDs scoped to our PIDs; do not synthesize global UI input."""

    def __init__(self):
        self.api = ctypes.WinDLL("user32", use_last_error=True)
        # Match screen/window sampling coordinates without changing either
        # product process or the user's display settings.
        if hasattr(self.api, "SetProcessDpiAwarenessContext"):
            self.api.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
            self.api.SetProcessDpiAwarenessContext.restype = wintypes.BOOL
            self.api.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        self.callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, ctypes.c_ssize_t)
        self.api.EnumWindows.argtypes = [self.callback_type, ctypes.c_ssize_t]
        self.api.EnumWindows.restype = wintypes.BOOL
        self.api.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        self.api.GetWindowThreadProcessId.restype = wintypes.DWORD
        self.api.IsWindowVisible.argtypes = [wintypes.HWND]
        self.api.IsWindowVisible.restype = wintypes.BOOL
        self.api.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
        self.api.GetDlgItem.restype = wintypes.HWND
        self.api.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
        self.api.GetAncestor.restype = wintypes.HWND
        self.api.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t]
        self.api.PostMessageW.restype = wintypes.BOOL
        self.api.SendMessageTimeoutW.argtypes = [
            wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t,
            wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t),
        ]
        self.api.SendMessageTimeoutW.restype = ctypes.c_ssize_t
        self.api.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        self.api.GetWindowTextW.restype = ctypes.c_int
        self.api.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        self.api.GetClassNameW.restype = ctypes.c_int
        self.api.EnumChildWindows.argtypes = [wintypes.HWND, self.callback_type, ctypes.c_ssize_t]
        self.api.EnumChildWindows.restype = wintypes.BOOL
        self.api.GetDlgCtrlID.argtypes = [wintypes.HWND]
        self.api.GetDlgCtrlID.restype = ctypes.c_int
        self.api.GetCursorPos.argtypes = [ctypes.POINTER(wintypes.POINT)]
        self.api.GetCursorPos.restype = wintypes.BOOL
        self.api.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
        self.api.SetCursorPos.restype = wintypes.BOOL
        self.api.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        self.api.GetClientRect.restype = wintypes.BOOL
        self.api.GetWindowDisplayAffinity.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        self.api.GetWindowDisplayAffinity.restype = wintypes.BOOL
        self.api.GetDC.argtypes = [wintypes.HWND]
        self.api.GetDC.restype = wintypes.HDC
        self.api.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
        self.api.ReleaseDC.restype = ctypes.c_int
        self.gdi = ctypes.WinDLL("gdi32", use_last_error=True)
        self.gdi.GetPixel.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
        self.gdi.GetPixel.restype = wintypes.DWORD

    def pid(self, window: int) -> int:
        value = wintypes.DWORD()
        self.api.GetWindowThreadProcessId(window, ctypes.byref(value))
        return value.value

    def windows(self, pid: int, visible: bool = True) -> list[int]:
        found = []

        @self.callback_type
        def collect(window, _):
            if self.pid(window) == pid and (not visible or self.api.IsWindowVisible(window)):
                found.append(window)
            return True

        self.api.EnumWindows(collect, 0)
        return found

    def title(self, window: int) -> str:
        value = ctypes.create_unicode_buffer(512)
        self.api.GetWindowTextW(window, value, len(value))
        return value.value

    def snapshot(self, pid: int) -> list[dict]:
        """Failure metadata only; exclude Edit controls (including passwords)."""
        result = []
        for window in self.windows(pid, visible=False):
            children = []

            @self.callback_type
            def collect(child, _):
                name = ctypes.create_unicode_buffer(128)
                self.api.GetClassNameW(child, name, len(name))
                detail = {"id": self.api.GetDlgCtrlID(child), "class": name.value}
                if name.value.lower() in {"static", "button"}:
                    detail["text"] = self.title(child)
                children.append(detail)
                return len(children) < 30

            self.api.EnumChildWindows(window, collect, 0)
            result.append({"title": self.title(window), "visible": bool(self.api.IsWindowVisible(window)), "controls": children})
        return result

    def send(self, window: int, message: int, wparam: int = 0, lparam: int = 0) -> int:
        result = ctypes.c_size_t()
        if not self.api.SendMessageTimeoutW(window, message, wparam, lparam, 2, 2000, ctypes.byref(result)):
            raise TimeoutError(f"Window message {message:#x} failed or timed out")
        return result.value

    def set_text(self, window: int, value: str) -> None:
        text = ctypes.create_unicode_buffer(value)
        self.send(window, 0x000C, 0, ctypes.cast(text, ctypes.c_void_p).value)

    def click(self, dialog: int, control_id: int) -> None:
        control = self.api.GetDlgItem(dialog, control_id)
        require(bool(control), f"Missing control {control_id}")
        # Post WM_COMMAND/BN_CLICKED: login handlers open modal dialogs and must
        # not block the automation thread in a synchronous BM_CLICK call.
        require(bool(self.api.PostMessageW(dialog, 0x0111, control_id, control)), "WM_COMMAND failed")

    def dialog(self, process: subprocess.Popen, ids: tuple[int, ...], timeout: float, titled: bool = False, expected_title: str | None = None) -> int:
        def find():
            require(process.poll() is None, f"PID {process.pid} exited early: {process.returncode}")
            for window in self.windows(process.pid):
                title = self.title(window)
                if expected_title is not None and title != expected_title:
                    continue
                if (not titled or title) and all(self.api.GetDlgItem(window, item) for item in ids):
                    return window
            return None

        try:
            return wait_until(find, timeout, f"Dialog with controls {ids} was not shown")
        except TimeoutError as exc:
            raise TimeoutError(f"{exc}; own process windows={self.snapshot(process.pid)!r}") from exc

    def cursor(self) -> tuple[int, int]:
        point = wintypes.POINT()
        require(bool(self.api.GetCursorPos(ctypes.byref(point))), "GetCursorPos failed")
        return point.x, point.y

    def client_size(self, window: int) -> tuple[int, int]:
        rectangle = wintypes.RECT()
        require(bool(self.api.GetClientRect(window, ctypes.byref(rectangle))), "GetClientRect failed")
        return rectangle.right - rectangle.left, rectangle.bottom - rectangle.top

    def display_affinity(self, window: int):
        window = self.api.GetAncestor(window, 2)  # GA_ROOT; children inherit WDA.
        value = wintypes.DWORD()
        return value.value if self.api.GetWindowDisplayAffinity(window, ctypes.byref(value)) else None

    def sampled_color_count(self, window: int) -> int:
        width, height = self.client_size(window)
        dc = self.api.GetDC(window)
        require(bool(dc), "GetDC failed")
        try:
            samples = [
                self.gdi.GetPixel(dc, width * x // 10, height * y // 8)
                for x in range(1, 10) for y in range(1, 8)
            ]
            colors = set(samples)
            colors.discard(0xFFFFFFFF)  # CLR_INVALID; never a pixel value.
            self.last_pixel_sample = {"distinct_colors": len(colors), "valid_samples": sum(value != 0xFFFFFFFF for value in samples),
                                      "total_samples": len(samples)}
            return len(colors)
        finally:
            self.api.ReleaseDC(window, dc)

    def close_windows(self, process: subprocess.Popen) -> None:
        if process.poll() is None:
            for window in self.windows(process.pid):
                self.api.PostMessageW(window, 0x0010, 0, 0)  # WM_CLOSE


class Peer:
    def __init__(self, port: int, timeout: float):
        self.timeout = timeout
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def exact(self, size: int, deadline: float) -> bytes:
        result = bytearray()
        while len(result) < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"Timed out receiving {size} bytes")
            self.sock.settimeout(remaining)
            part = self.sock.recv(min(size - len(result), 65536))
            if not part:
                raise EOFError("Peer closed the connection")
            result.extend(part)
        return bytes(result)

    def message(self, timeout: float | None = None):
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        kind = self.exact(1, deadline)[0]
        if kind == SETUP:
            return kind, struct.unpack("<iii", self.exact(12, deadline))
        if kind == IMAGE:
            left, top, right, bottom, size = struct.unpack("<iiiiI", self.exact(20, deadline))
            require(0 < size <= 32 * 1024 * 1024, f"Invalid JPEG length: {size}")
            return kind, ((left, top, right, bottom), self.exact(size, deadline))
        raise AssertionError(f"Unexpected server message type: {kind}")

    def expect_permission(self, category: int, value: int) -> None:
        kind, fields = self.message()
        require((kind, fields) == (SETUP, (PERMISSION, category, value)), f"Unexpected permission reply: {(kind, fields)}")

    def login(self, username: str, password: str = PASSWORD, fragmented: bool = False) -> int:
        self.expect_permission(IP, 1)
        packet = struct.pack("<B20s20s", USER, username.encode("ascii"), password.encode("ascii"))
        if fragmented:
            offset = 0
            for count in (1, 2, 3, 5, 8, 13, 9):
                self.sock.sendall(packet[offset:offset + count])
                offset += count
                time.sleep(0.02)
            require(offset == len(packet), "Invalid test fragmentation")
        else:
            self.sock.sendall(packet)
        kind, fields = self.message()
        require(kind == SETUP and fields[:2] == (PERMISSION, CONTROL), "Missing account permission reply")
        return fields[2]


def jpeg_dimensions(data: bytes) -> tuple[int, int]:
    require(data.startswith(b"\xff\xd8") and data.endswith(b"\xff\xd9"), "Invalid JPEG boundary markers")
    position = 2
    sof_markers = {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}
    while position < len(data):
        require(data[position] == 0xFF, "Malformed JPEG marker")
        while position < len(data) and data[position] == 0xFF:
            position += 1
        require(position < len(data), "Truncated JPEG marker")
        marker = data[position]
        position += 1
        if marker in {0xD8, 0xD9, 0x01} or 0xD0 <= marker <= 0xD7:
            continue
        require(position + 2 <= len(data), "Truncated JPEG segment")
        length = int.from_bytes(data[position:position + 2], "big")
        require(length >= 2 and position + length <= len(data), "Invalid JPEG segment length")
        if marker in sof_markers:
            require(length >= 8, "Invalid JPEG frame header")
            height, width = struct.unpack(">HH", data[position + 3:position + 7])
            return width, height
        if marker == 0xDA:
            break
        position += length
    raise AssertionError("JPEG has no supported frame header")


def receive_tiles(peer: Peer, count: int = 3) -> dict:
    width = height = 0
    byte_count = received = dimension_updates = 0
    deadline = time.monotonic() + peer.timeout
    while received < count:
        remaining = deadline - time.monotonic()
        require(remaining > 0, "Timed out waiting for JPEG tiles")
        kind, payload = peer.message(timeout=remaining)
        if kind == SETUP:
            require(payload[0] == PIXEL, f"Unexpected setup while receiving images: {payload}")
            width, height = payload[1:]
            require(0 < width <= 32768 and 0 < height <= 32768, "Invalid desktop dimensions")
            dimension_updates += 1
            # Login and the first capture can both announce the initial size.
            require(dimension_updates <= 8, "Excessive dimension updates without image progress")
            continue
        require(kind == IMAGE and width > 0, "Image received before screen dimensions")
        (left, top, right, bottom), jpeg = payload
        require(0 <= left < right <= width and 0 <= top < bottom <= height, "Tile outside desktop")
        require(jpeg_dimensions(jpeg) == (right - left, bottom - top), "JPEG and rectangle dimensions differ")
        byte_count += len(jpeg)
        received += 1
    return {"desktop": [width, height], "validated_jpeg_tiles": received, "jpeg_bytes": byte_count, "dimension_updates": dimension_updates}


def wide_field(value: str, size: int) -> bytes:
    data = value.encode("utf-16-le")
    require(len(data) < size * 2, "Fixture string too long")
    return data.ljust(size * 2, b"\0")


def create_fixtures(root: Path, port: int) -> tuple[Path, Path]:
    server, client = root / "server", root / "client"
    server.mkdir()
    client.mkdir()
    (server / "RemoteConfig.ini").write_text(
        "[sets]\nallowControl=1\nautoRun=0\nshowTask=0\ncodingFile=0\n"
        f"[admin]\nusePwdProtect=1\nadminPwd={ADMIN_PASSWORD}\n[connection]\nmainPort={port}\n",
        encoding="ascii",
    )
    # Win32 Unicode UserData: int power; WCHAR name[20]; WCHAR pwd[20].
    (server / "user.plain").write_bytes(b"".join(
        struct.pack("<i40s40s", power, wide_field(name, 20), wide_field(PASSWORD, 20))
        for name, power in (("view", 0), ("control", 1))
    ))
    (server / "IpRule.dat").write_bytes(b"")
    (client / "ClientConfig.xml").write_text(
        '<?xml version="1.0" encoding="gb2312" ?>\n<ClientConifg><HostList>'
        f'<Host Name="SmokeLocal" Ip="127.0.0.1" Port="{port}" />'
        '</HostList><Setting><ImageQuality>60</ImageQuality><Frame>10</Frame></Setting></ClientConifg>\n',
        encoding="ascii",
    )
    return server, client


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True, help="Directory containing Server.exe and Client.exe")
    parser.add_argument("--timeout", type=float, default=12, help="Per-operation timeout in seconds")
    parser.add_argument("--skip-client", action="store_true", help="Skip only the native Client GUI test")
    parser.add_argument("--keep-data", action="store_true", help="Keep isolated fixtures after success (always kept on failure)")
    parser.add_argument("--report", type=Path, help="Optional JSON report path; never contains captured pixels")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This test requires an interactive Windows desktop")
    if not 1 <= args.timeout <= 120:
        parser.error("--timeout must be between 1 and 120 seconds")
    binary_dir = args.bin_dir.resolve()
    for name in ("Server.exe",) if args.skip_client else ("Server.exe", "Client.exe"):
        if not (binary_dir / name).is_file():
            parser.error(f"Executable not found: {binary_dir / name}")

    ui = WinUI()
    root = Path(tempfile.mkdtemp(prefix="rcs-runtime-smoke-")).resolve()
    processes: list[subprocess.Popen] = []
    cases = []
    success = False
    report = {"binary_directory": str(binary_dir), "temporary_directory": str(root), "cases": cases}

    def record(name: str, details=None, status="pass"):
        cases.append({"name": name, "status": status, "details": details or {}})
        print(json.dumps(cases[-1], ensure_ascii=True), flush=True)

    def launch(name: str, data_dir: Path):
        process = subprocess.Popen([str(binary_dir / name), "--data-dir", str(data_dir)], cwd=data_dir)
        processes.append(process)
        return process

    def close_cleanly(process: subprocess.Popen, name: str, main_window: int):
        require(ui.pid(main_window) == process.pid, "Close target is not our process")
        require(bool(ui.api.PostMessageW(main_window, 0x0010, 0, 0)), "WM_CLOSE failed")
        code = process.wait(timeout=args.timeout)
        # MFC can propagate IDCANCEL (2). Server's outer authentication dialog
        # also completes with IDOK (1) before its nested main dialog is closed.
        # Only these identified normal dialog results are accepted; crash codes
        # and every other exit value remain failures.
        accepted = (0, 1, 2) if name == "server" else (0, 2)
        require(code in accepted, f"{name} exited with {code}")
        record(name + "_graceful_exit", {"pid": process.pid, "exit_code": code})

    try:
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        report["port"] = port
        server_data, client_data = create_fixtures(root, port)
        server = launch("Server.exe", server_data)
        login = ui.dialog(server, (1017, 1), args.timeout)
        ui.set_text(ui.api.GetDlgItem(login, 1017), ADMIN_PASSWORD)
        ui.click(login, 1)
        server_window = ui.dialog(server, (1000, 1007), args.timeout)
        ui.click(server_window, 1007)

        def listening():
            require(server.poll() is None, "Server exited while starting")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return True
            except OSError:
                return False

        wait_until(listening, args.timeout, "Server did not start listening")
        record("server_gui_start", {"pid": server.pid, "port": port})

        with Peer(port, args.timeout) as viewing:
            require(viewing.login("view", fragmented=True) == 0, "Viewing account permission was not 0")
            image_info = receive_tiles(viewing)
            record("fragmented_view_login_and_jpeg", image_info)
            with Peer(port, args.timeout) as wrong:
                require(wrong.login("view", "wrong-password") == -1, "Wrong password was accepted")
                record("wrong_password_rejected")
            with Peer(port, args.timeout) as duplicate:
                require(duplicate.login("view") == 2, "Duplicate account was accepted")
                record("duplicate_account_rejected")

            before = ui.cursor()
            width, height = image_info["desktop"]
            target = (max(0, width // 3), max(0, height // 3))
            if target == before:
                target = (max(0, width * 2 // 3), max(0, height * 2 // 3))
            observed = []
            try:
                viewing.sock.sendall(struct.pack("<Biii", MOUSE, target[0], target[1], 0))
                for _ in range(10):
                    time.sleep(0.05)
                    observed.append(ui.cursor())
                require(target not in observed, "Viewing account moved the system cursor")
                if any(point != before for point in observed):
                    record("view_input_rejected", {"reason": "Cursor moved independently; permission observation is inconclusive"}, "skip")
                else:
                    record("view_input_rejected", {"cursor_unchanged": True})
            finally:
                # Restore a cursor moved by our test, without undoing unrelated
                # movement by the user during the observation interval.
                if target in observed and ui.cursor() == target:
                    ui.api.SetCursorPos(*before)

        def reconnected():
            with Peer(port, args.timeout) as peer:
                power = peer.login("view")
                if power == 2:
                    return False
                require(power == 0, f"Reconnect returned permission {power}")
                receive_tiles(peer, count=1)
                return True

        wait_until(reconnected, args.timeout, "Disconnected account was not released")
        record("disconnect_and_reconnect")

        with Peer(port, args.timeout) as control:
            require(control.login("control", fragmented=True) == 1, "Control account permission was not 1")
            record("control_authorization_and_jpeg", receive_tiles(control, count=1))
        record("control_keyboard_input", {"reason": "Not sent: SendInput targets the global foreground window; authorization and packet flow tested instead"}, "skip")

        rules_path = server_data / "IpRule.dat"
        rules_path.write_bytes(struct.pack("<32s32si", wide_field("127.0.0.1", 16), wide_field("127.0.0.1", 16), 0))
        try:
            with Peer(port, args.timeout) as denied:
                denied.expect_permission(IP, 0)
            record("loopback_ip_rule_rejected")
        finally:
            rules_path.write_bytes(b"")

        # Ensure account cleanup has completed before the GUI logs into it.
        wait_until(reconnected, args.timeout, "Viewing account still connected before Client GUI test")
        time.sleep(0.3)
        if not args.skip_client:
            client = launch("Client.exe", client_data)
            dialog = ui.dialog(client, (1000, 1012, 1003, 1004), args.timeout)
            combo = ui.api.GetDlgItem(dialog, 1000)
            require(ui.send(combo, 0x014E, 1) != ctypes.c_size_t(-1).value, "Client has no configured host")  # CB_SETCURSEL
            ui.set_text(ui.api.GetDlgItem(dialog, 1012), "view")
            ui.set_text(ui.api.GetDlgItem(dialog, 1003), PASSWORD)
            ui.click(dialog, 1004)
            # The owned DisplayDlg popup also contains control 1028; the main
            # dialog is the titled top-level window, not that drawing surface.
            client_window = ui.dialog(client, (), args.timeout, titled=True, expected_title="Remote Control Client")
            time.sleep(1)
            require(client.poll() is None and ui.api.IsWindowVisible(client_window), "Client main window did not stay visible")
            stale_logins = [window for window in ui.windows(client.pid)
                            if all(ui.api.GetDlgItem(window, item) for item in (1000, 1012, 1003, 1004))]
            require(not stale_logins, f"Login dialog remained visible after authentication: {ui.snapshot(client.pid)!r}")
            record("client_gui_view_login", {"pid": client.pid, "main_window_visible": True, "login_dialog_visible": False,
                                             "main_client_size": list(ui.client_size(client_window))})

            def drawing_surface():
                display = ui.api.GetDlgItem(client_window, 4100)
                if display:
                    surface = ui.api.GetDlgItem(display, 1028)
                    if surface:
                        return display, surface
                for window in ui.windows(client.pid):
                    if window != client_window and not ui.title(window):
                        surface = ui.api.GetDlgItem(window, 1028)
                        if surface:
                            return window, surface
                return None

            display_window, surface = wait_until(drawing_surface, args.timeout, "Native DisplayDlg drawing surface was not shown")
            if ui.api.GetDlgItem(client_window, 4100):
                require(ui.send(surface, 0x8340) == 0, "Normal Client unexpectedly enabled the opt-in paint probe")
                record("paint_probe_disabled_by_default")
            display_size, surface_size = ui.client_size(display_window), ui.client_size(surface)
            require(surface_size == display_size and surface_size[0] > 24 and surface_size[1] > 15,
                    f"Drawing surface did not fill its window: display={display_size}, surface={surface_size}")
            require(bool(ui.api.IsWindowVisible(surface)), "Drawing surface is hidden")
            record("client_image_surface_layout", {"display_client_size": list(display_size), "surface_client_size": list(surface_size),
                                                    "surface_visible": True})
            color_count = ui.sampled_color_count(surface)
            affinity = ui.display_affinity(display_window)
            if affinity == 0x11 and ui.last_pixel_sample["valid_samples"] == 0:
                # A paired synthetic-window probe confirms: the owner can read
                # its painted pixels, while another process gets CLR_INVALID
                # only while WDA_EXCLUDEFROMCAPTURE is enabled. Do not label
                # this external readback as successful rendering verification.
                record("client_image_surface_pixels", {"reason": "WDA_EXCLUDEFROMCAPTURE blocks cross-process GetPixel; use the separate wire-pixel exclusion regression",
                                                         "affinity": affinity, "get_pixel_return": "CLR_INVALID (0xFFFFFFFF)",
                                                         **ui.last_pixel_sample}, "skip")
            else:
                try:
                    color_count = wait_until(lambda: (value if (value := ui.sampled_color_count(surface)) > 1 else None),
                                             args.timeout, "DisplayDlg pixels could not be verified after receiving images")
                except TimeoutError as exc:
                    raise TimeoutError(f"{exc}; surface_size={surface_size}; affinity={affinity}; pixel_read={ui.last_pixel_sample}") from exc
                record("client_image_surface_pixels", {"grid_sample_color_count": color_count, "image_saved": False})
            close_cleanly(client, "client", client_window)
        else:
            record("client_gui_view_login", {"reason": "--skip-client supplied"}, "skip")

        # Stop listener using its own button, then close our Server window.
        ui.click(server_window, 1007)

        def listener_stopped():
            require(server.poll() is None, "Server exited instead of stopping its listener")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return False
            except OSError:
                return True

        wait_until(listener_stopped, args.timeout, "Server listener remained reachable after Stop")
        record("server_listener_stopped")
        close_cleanly(server, "server", server_window)
        success = True
    except Exception as exc:
        record("runtime_smoke", {"error": str(exc), "exception": type(exc).__name__}, "fail")
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                ui.close_windows(process)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    # Popen holds the handle of our own process; never kill by
                    # executable name or enumerate/terminate unrelated processes.
                    process.terminate()
                    process.wait(timeout=3)
                    record("cleanup_forced_exit", {"pid": process.pid}, "cleanup")
        report["success"] = success
        report["data_retained"] = args.keep_data or not success
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
        if success and not args.keep_data:
            # Only remove the fresh directory created by this invocation.
            require(root.parent == Path(tempfile.gettempdir()).resolve() and root.name.startswith("rcs-runtime-smoke-"), "Unsafe temporary cleanup path")
            shutil.rmtree(root)
        print(json.dumps(report, ensure_ascii=True), flush=True)
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
