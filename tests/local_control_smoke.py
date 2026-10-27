"""Opt-in end-to-end local control test with the real Client and Server.

This is intentionally different from the individual packet/injector tests.
Physical mouse input goes only to our Client; a native LISTBOX underneath it
must receive the mapped virtual input and open a real temporary fixture file.
No test mouse/button messages are posted directly to the target control.

Run in an exclusive interactive slot after building:
  python tests/local_control_smoke.py --bin-dir build-v1.1.1/bin/Release --allow-input

The physical pointer is restored and only this run's processes/windows/files
are cleaned up. No keyboard input, user files, Explorer windows, or saved
desktop images are involved.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
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
import uuid

from runtime_smoke import ADMIN_PASSWORD, PASSWORD, create_fixtures, require, wait_until
from capture_exclusion_smoke import SyntheticWindow
from ui_modes_smoke import ModeUI, DISPLAY_ID, CANVAS_ID, BAR_ID, FIT_ID


FILE_LIST_ID, CONTENT_ID = 6101, 6102
MOVE, LEFT_DOWN, LEFT_UP, LEFT_DOUBLE, LEAVE, CAPTURE_CHANGED = 0x200, 0x201, 0x202, 0x203, 0x2A3, 0x215


class NativeFileTarget:
    """Real Windows LISTBOX semantics, including its ordinary SetCapture calls."""

    def __init__(self, ui, directory: Path, rectangle):
        self.ui, self.directory, self.rectangle = ui, directory, rectangle
        self.paths = [directory / "alpha.txt", directory / "beta.txt"]
        for index, path in enumerate(self.paths):
            path.write_text(f"RCS actual file open fixture {index}: {uuid.uuid4().hex}\n", encoding="utf-8")
        self.events, self.opens = [], []
        self.lock = threading.RLock()
        self.window = self.listbox = self.content = None
        self.error = None
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        require(self.ready.wait(5), "Native file target did not initialize")
        if self.error:
            raise self.error
        require(self.window and self.listbox and self.content, "Native file target is incomplete")

    def snapshot(self):
        with self.lock:
            return list(self.events), list(self.opens)

    def _run(self):
        api = ctypes.WinDLL("user32", use_last_error=True)
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        gdi = ctypes.WinDLL("gdi32")
        procedure_type = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t)

        class WindowClass(ctypes.Structure):
            _fields_ = [("style", wintypes.UINT), ("procedure", procedure_type), ("class_extra", ctypes.c_int),
                        ("window_extra", ctypes.c_int), ("instance", wintypes.HINSTANCE), ("icon", wintypes.HICON),
                        ("cursor", wintypes.HANDLE), ("background", wintypes.HBRUSH),
                        ("menu_name", wintypes.LPCWSTR), ("class_name", wintypes.LPCWSTR)]

        kernel.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
        kernel.GetModuleHandleW.restype = wintypes.HMODULE
        api.RegisterClassW.argtypes = [ctypes.POINTER(WindowClass)]
        api.RegisterClassW.restype = wintypes.ATOM
        api.UnregisterClassW.argtypes = [wintypes.LPCWSTR, wintypes.HINSTANCE]
        api.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
                                       ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                       wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, ctypes.c_void_p]
        api.CreateWindowExW.restype = wintypes.HWND
        api.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t]
        api.DefWindowProcW.restype = ctypes.c_ssize_t
        api.SendMessageW.argtypes = api.DefWindowProcW.argtypes
        api.SendMessageW.restype = ctypes.c_ssize_t
        api.CallWindowProcW.argtypes = [ctypes.c_void_p, wintypes.HWND, wintypes.UINT, ctypes.c_size_t, ctypes.c_ssize_t]
        api.CallWindowProcW.restype = ctypes.c_ssize_t
        api.SetWindowLongPtrW.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t]
        api.SetWindowLongPtrW.restype = ctypes.c_ssize_t
        api.GetCapture.restype = wintypes.HWND
        api.GetCursorPos.argtypes = [ctypes.POINTER(wintypes.POINT)]
        api.GetMessageExtraInfo.restype = ctypes.c_ssize_t
        api.SetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]
        api.MoveWindow.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.BOOL]
        api.DestroyWindow.argtypes = [wintypes.HWND]
        api.PostQuitMessage.argtypes = [ctypes.c_int]
        api.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT]
        api.GetMessageW.restype = ctypes.c_int
        api.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
        api.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
        api.DispatchMessageW.restype = ctypes.c_ssize_t
        gdi.GetStockObject.argtypes = [ctypes.c_int]
        gdi.GetStockObject.restype = wintypes.HANDLE
        old_list_procedure = None

        def layout():
            if self.listbox and self.content:
                width, height = self.ui.client_size(self.window)
                api.MoveWindow(self.listbox, 8, 8, max(50, width - 16), max(80, height // 2), True)
                api.MoveWindow(self.content, 8, height // 2 + 20, max(50, width - 16), max(40, height // 2 - 28), True)

        @procedure_type
        def list_procedure(window, message, wparam, lparam):
            observed = message in (MOVE, LEFT_DOWN, LEFT_UP, LEFT_DOUBLE, LEAVE, CAPTURE_CHANGED)
            if observed:
                cursor = wintypes.POINT()
                api.GetCursorPos(ctypes.byref(cursor))
                entry = {"message": message, "flags": int(wparam),
                         "local_point": [ctypes.c_short(lparam & 0xFFFF).value, ctypes.c_short((lparam >> 16) & 0xFFFF).value],
                         "physical_cursor": [cursor.x, cursor.y], "capture_before": int(api.GetCapture() or 0),
                         "extra_info": api.GetMessageExtraInfo() & 0xFFFFFFFF,
                         "new_capture": int(lparam) if message == CAPTURE_CHANGED else None,
                         "time": time.monotonic()}
            result = api.CallWindowProcW(old_list_procedure, window, message, wparam, lparam)
            if observed:
                entry["capture_after"] = int(api.GetCapture() or 0)
                with self.lock:
                    self.events.append(entry)
            return result

        @procedure_type
        def procedure(window, message, wparam, lparam):
            try:
                if message == 0x0005:
                    layout()
                elif message == 0x0111 and (wparam & 0xFFFF) == FILE_LIST_ID and ((wparam >> 16) & 0xFFFF) in (1, 2):
                    index = api.SendMessageW(self.listbox, 0x0188, 0, 0)  # LB_GETCURSEL / LBN_DBLCLK
                    notification = (wparam >> 16) & 0xFFFF
                    with self.lock:
                        self.events.append({"message": 0x0111, "notification": notification, "selected_index": int(index),
                                            "capture_after": int(api.GetCapture() or 0), "time": time.monotonic()})
                    if notification == 1:  # LBN_SELCHANGE is an actual control-state observation.
                        return 0
                    if 0 <= index < len(self.paths):
                        path = self.paths[index].resolve()
                        require(path.parent == self.directory.resolve(), "Target file is outside this fixture")
                        data = path.read_bytes()
                        text = data.decode("utf-8")
                        api.SetWindowTextW(self.content, text)
                        with self.lock:
                            self.opens.append({"file": path.name, "bytes_read": len(data),
                                               "sha256": hashlib.sha256(data).hexdigest(), "time": time.monotonic()})
                    return 0
                elif message == 0x0010:
                    api.DestroyWindow(window)
                    return 0
                elif message == 0x0002:
                    api.PostQuitMessage(0)
                    return 0
            except Exception as exc:
                self.error = str(exc)
            return api.DefWindowProcW(window, message, wparam, lparam)

        name = "RcsNativeFileList-" + uuid.uuid4().hex
        instance = kernel.GetModuleHandleW(None)
        klass = WindowClass(0x0008, procedure, 0, 0, instance, None, None, wintypes.HBRUSH(6), None, name)
        try:
            require(bool(api.RegisterClassW(ctypes.byref(klass))), "Register file target class failed")
            left, top, right, bottom = self.rectangle
            self.window = api.CreateWindowExW(0x88, name, "RCS own temporary files", 0x10CF0000,
                                              left, top, right - left, bottom - top, None, None, instance, None)
            require(bool(self.window), "Create file target failed")
            self.listbox = api.CreateWindowExW(0, "LISTBOX", "", 0x50810101,
                                               8, 8, 400, 200, self.window, FILE_LIST_ID, instance, None)
            self.content = api.CreateWindowExW(0, "EDIT", "Double-click one of this test's files.", 0x50A00804,
                                               8, 230, 400, 130, self.window, CONTENT_ID, instance, None)
            require(self.listbox and self.content, "Create native target controls failed")
            font = gdi.GetStockObject(17)  # DEFAULT_GUI_FONT, not owned by us.
            api.SendMessageW(self.listbox, 0x0030, font, 1)
            api.SendMessageW(self.content, 0x0030, font, 1)
            old_list_procedure = api.SetWindowLongPtrW(self.listbox, -4, ctypes.cast(list_procedure, ctypes.c_void_p).value)
            require(bool(old_list_procedure), "Subclass native listbox failed")
            for path in self.paths:
                text = ctypes.create_unicode_buffer(path.name)
                api.SendMessageW(self.listbox, 0x0180, 0, ctypes.cast(text, ctypes.c_void_p).value)
            layout()
            self.ready.set()
            message = wintypes.MSG()
            while api.GetMessageW(ctypes.byref(message), None, 0, 0) > 0:
                api.TranslateMessage(ctypes.byref(message))
                api.DispatchMessageW(ctypes.byref(message))
        except Exception as exc:
            self.error = str(exc)
            self.ready.set()
        finally:
            api.UnregisterClassW(name, instance)

    def item_point(self, index=0, fraction=0.3):
        rectangle = wintypes.RECT()
        require(self.ui.send(self.listbox, 0x0198, index, ctypes.cast(ctypes.byref(rectangle), ctypes.c_void_p).value) != ctypes.c_size_t(-1).value,
                "LB_GETITEMRECT failed")
        left, top, _, _ = self.ui.screen_client_rect(self.listbox)
        return (left + rectangle.left + int((rectangle.right - rectangle.left) * fraction),
                top + (rectangle.top + rectangle.bottom) // 2)

    def displayed_text(self):
        text = ctypes.create_unicode_buffer(1024)
        self.ui.send(self.content, 0x000D, len(text), ctypes.cast(text, ctypes.c_void_p).value)
        return text.value

    def selected_index(self):
        return ctypes.c_int(self.ui.send(self.listbox, 0x0188) & 0xFFFFFFFF).value

    def close(self):
        if self.window and self.thread.is_alive():
            require(self.ui.pid(self.window) == os.getpid(), "Target cleanup does not belong to the test")
            self.ui.api.PostMessageW(self.window, 0x0010, 0, 0)
            self.thread.join(timeout=5)
            require(not self.thread.is_alive(), "Native file target did not stop")


class MouseInput(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("data", wintypes.DWORD),
                ("flags", wintypes.DWORD), ("time", wintypes.DWORD), ("extra", ctypes.c_size_t)]


class Input(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("mouse", MouseInput)]


class LocalInputUI(ModeUI):
    def __init__(self):
        super().__init__()
        self.api.WindowFromPoint.argtypes = [wintypes.POINT]
        self.api.WindowFromPoint.restype = wintypes.HWND
        self.api.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(Input), ctypes.c_int]
        self.api.SendInput.restype = wintypes.UINT
        self.api.GetAsyncKeyState.argtypes = [ctypes.c_int]
        self.api.GetAsyncKeyState.restype = ctypes.c_short
        self.api.GetDoubleClickTime.restype = wintypes.UINT
        self.left_held = False
        require(ctypes.sizeof(Input) == (40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28), "Unexpected INPUT layout")

    def hit_is_client(self, client, point, canvas=None):
        hit = self.api.WindowFromPoint(wintypes.POINT(*point))
        require(client.poll() is None and hit and self.pid(hit) == client.pid, "Refusing input outside our live Client")
        if canvas:
            require(hit == canvas, "Physical input point is not on our exact canvas")
        return hit

    def move_physical(self, client, point, canvas=None):
        self.hit_is_client(client, point, canvas)
        left, top = self.api.GetSystemMetrics(76), self.api.GetSystemMetrics(77)
        width, height = self.api.GetSystemMetrics(78), self.api.GetSystemMetrics(79)
        x = ((point[0] - left) * 65536 + 32768) // width
        y = ((point[1] - top) * 65536 + 32768) // height
        event = Input(0, MouseInput(min(65535, max(0, x)), min(65535, max(0, y)), 0, 0xE001, 0, 0))
        require(self.api.SendInput(1, ctypes.byref(event), ctypes.sizeof(event)) == 1, "Physical movement failed")

    def button(self, client, canvas, down, require_canvas=True):
        self.hit_is_client(client, self.cursor(), canvas if require_canvas else None)
        event = Input(0, MouseInput(0, 0, 0, 0x0002 if down else 0x0004, 0, 0))
        inserted = self.api.SendInput(1, ctypes.byref(event), ctypes.sizeof(event))
        if inserted:
            self.left_held = down
        require(inserted == 1, "Physical button transition failed")

    def emergency_release(self):
        if self.left_held:
            event = Input(0, MouseInput(0, 0, 0, 0x0004, 0, 0))
            self.api.SendInput(1, ctypes.byref(event), ctypes.sizeof(event))
            self.left_held = False

    def thread_capture(self, window):
        class GuiThreadInfo(ctypes.Structure):
            _fields_ = [("size", wintypes.DWORD), ("flags", wintypes.DWORD), ("active", wintypes.HWND),
                        ("focus", wintypes.HWND), ("capture", wintypes.HWND), ("menu_owner", wintypes.HWND),
                        ("move_size", wintypes.HWND), ("caret", wintypes.HWND), ("caret_rect", wintypes.RECT)]
        self.api.GetGUIThreadInfo.argtypes = [wintypes.DWORD, ctypes.POINTER(GuiThreadInfo)]
        self.api.GetGUIThreadInfo.restype = wintypes.BOOL
        thread_id = self.api.GetWindowThreadProcessId(window, None)
        info = GuiThreadInfo()
        info.size = ctypes.sizeof(info)
        require(bool(self.api.GetGUIThreadInfo(thread_id, ctypes.byref(info))), "GetGUIThreadInfo failed for an owned window")
        return int(info.capture or 0)


def point_on_canvas(ui, canvas, remote_point):
    left, top, right, bottom = ui.screen_client_rect(canvas)
    width, height = right - left, bottom - top
    scale = min(width / ui.width, height / ui.height)
    image_width = max(1, math.floor(ui.width * scale + 0.5))
    image_height = max(1, math.floor(ui.height * scale + 0.5))
    return (left + (width - image_width) // 2 + round(remote_point[0] * (image_width - 1) / (ui.width - 1)),
            top + (height - image_height) // 2 + round(remote_point[1] * (image_height - 1) / (ui.height - 1)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--allow-input", action="store_true", help="Explicitly permit input restricted to this run's windows")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--keep-data", action="store_true")
    args = parser.parse_args()
    if os.name != "nt" or not args.allow_input:
        parser.error("Use an exclusive Windows desktop test slot and explicitly pass --allow-input")
    binary_dir = args.bin_dir.resolve()
    for name in ("Client.exe", "Server.exe"):
        require((binary_dir / name).is_file(), f"Missing real executable: {name}")
    ui = LocalInputUI()
    root = Path(tempfile.mkdtemp(prefix="rcs-local-control-")).resolve()
    original_cursor = ui.cursor()
    processes, cases = [], []
    target = guard = None
    success = False
    result = {"success": False, "real_client_and_server": True, "direct_target_input_messages_sent_by_test": 0,
              "temporary_directory": str(root), "cases": cases, "screenshots_saved": False}

    def record(name, **details):
        cases.append({"name": name, "status": "pass", "details": details})
        print(json.dumps(cases[-1], ensure_ascii=True), flush=True)

    def launch(name, data):
        process = subprocess.Popen([str(binary_dir / name), "--data-dir", str(data)], cwd=data)
        processes.append(process)
        return process

    def login_client(data, account):
        process = launch("Client.exe", data)
        dialog = ui.dialog(process, (1000, 1012, 1003, 1004), args.timeout)
        ui.raise_own(process, dialog)
        ui.send(ui.api.GetDlgItem(dialog, 1000), 0x014E, 1)
        ui.set_text(ui.api.GetDlgItem(dialog, 1012), account)
        ui.set_text(ui.api.GetDlgItem(dialog, 1003), PASSWORD)
        ui.click(dialog, 1004)
        main_window = ui.dialog(process, (DISPLAY_ID, BAR_ID), args.timeout)
        ui.place_own(process, main_window, ui.width // 6, ui.height // 6, ui.width * 2 // 3, ui.height * 2 // 3)
        canvas = ui.api.GetDlgItem(ui.api.GetDlgItem(main_window, DISPLAY_ID), CANVAS_ID)
        bar = ui.api.GetDlgItem(main_window, BAR_ID)
        ui.click(bar, FIT_ID)
        wait_until(lambda: ui.paint_count(canvas) > 0, args.timeout, "Real Server frames did not reach Client painting")
        time.sleep(0.3)
        return process, main_window, canvas, bar

    def cursor_stays(point):
        time.sleep(0.15)
        require(max(abs(a - b) for a, b in zip(ui.cursor(), point)) <= 1,
                f"Combined same-desktop control warped physical cursor from {point} to {ui.cursor()}")

    def physical_double_click(client, canvas, point):
        ui.move_physical(client, point, canvas)
        cursor_stays(point)
        interval = min(0.035, max(0.001, ui.api.GetDoubleClickTime() / 8000))
        for _ in range(2):
            ui.button(client, canvas, True)
            time.sleep(interval)
            ui.button(client, canvas, False)
            time.sleep(interval)
            require(max(abs(a - b) for a, b in zip(ui.cursor(), point)) <= 1, "Double-click warped away from our physical canvas point")
        cursor_stays(point)

    try:
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        server_data, client_data = create_fixtures(root, port)
        server = launch("Server.exe", server_data)
        auth = ui.dialog(server, (1017, 1), args.timeout)
        ui.set_text(ui.api.GetDlgItem(auth, 1017), ADMIN_PASSWORD)
        ui.click(auth, 1)
        server_window = ui.dialog(server, (1000, 1007), args.timeout)
        ui.click(server_window, 1007)

        def listening():
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return True
            except OSError:
                return False
        wait_until(listening, args.timeout, "Real Server did not listen")
        guard = SyntheticWindow(ui, (0, 0, ui.width, ui.height))
        files = root / "files"
        files.mkdir()
        x, y = ui.width // 6 + 90, ui.height // 6 + 150
        target = NativeFileTarget(ui, files, (x, y, x + 760, y + 480))

        viewer, view_window, canvas, _ = login_client(client_data, "view")
        q = target.item_point(0)
        p = point_on_canvas(ui, canvas, q)
        ui.hit_is_client(viewer, q)  # Target Q really is covered by Client.
        require(abs(p[0] - q[0]) + abs(p[1] - q[1]) >= 40, "Input P and mapped target Q are not sufficiently distinct")
        before_events, before_opens = target.snapshot()
        before_selection = target.selected_index()
        physical_double_click(viewer, canvas, p)
        time.sleep(0.3)
        after_events, after_opens = target.snapshot()
        require(target.selected_index() == before_selection and len(after_opens) == len(before_opens) and not any(e["message"] in (LEFT_DOWN, LEFT_UP, LEFT_DOUBLE, 0x0111)
                for e in after_events[len(before_events):]), "Viewing mode operated the underlying native listbox")
        record("viewing_mode_cannot_open_file", physical_point=list(p), covered_target_point=list(q))
        ui.api.PostMessageW(view_window, 0x0010, 0, 0)
        require(viewer.wait(timeout=args.timeout) in (0, 2), "Viewer did not close normally")

        client, main_window, canvas, bar = login_client(client_data, "control")
        mode_label = ui.api.GetDlgItem(bar, 4105)
        wait_until(lambda: mode_label and "同机控制" in ui.title(mode_label), args.timeout, "Client did not advertise verified same-desktop control")
        q = target.item_point(0)
        p = point_on_canvas(ui, canvas, q)
        ui.hit_is_client(client, q)
        ui.hit_is_client(client, p, canvas)
        record("real_pair_local_mode", server_pid=server.pid, client_pid=client.pid, mode_label=ui.title(mode_label),
               physical_point=list(p), covered_target_point=list(q))

        for index, fraction in enumerate((0.25, 0.35, 0.45)):
            q = target.item_point(0, fraction)
            p = point_on_canvas(ui, canvas, q)
            before = len(target.snapshot()[0])
            ui.move_physical(client, p, canvas)
            origin = ui.screen_client_rect(target.listbox)
            expected = (q[0] - origin[0], q[1] - origin[1])
            event = wait_until(lambda: next((e for e in reversed(target.snapshot()[0][before:]) if e["message"] == MOVE and
                                abs(e["local_point"][0] - expected[0]) <= 3 and abs(e["local_point"][1] - expected[1]) <= 3), None),
                               args.timeout, "Real Client+Server did not move over the mapped native item")
            cursor_stays(p)
            record(f"continuous_mapped_move_{index + 1}", physical_point=list(p), target_point=list(q), observed=event)

        fit = ui.api.GetDlgItem(bar, FIT_ID)
        bounds = ui.rect(fit)
        outside = ((bounds[0] + bounds[2]) // 2, (bounds[1] + bounds[3]) // 2)
        before = len(target.snapshot()[0])
        ui.move_physical(client, outside)
        time.sleep(0.2)
        require(not any(e["message"] == MOVE for e in target.snapshot()[0][before:]), "Moving out of canvas still controlled the file target")
        cursor_stays(outside)
        q = target.item_point(0)
        p = point_on_canvas(ui, canvas, q)
        before = len(target.snapshot()[0])
        ui.move_physical(client, p, canvas)
        wait_until(lambda: any(e["message"] == MOVE for e in target.snapshot()[0][before:]), args.timeout, "Reentering canvas did not resume target hover")
        cursor_stays(p)
        record("canvas_exit_and_reentry", physical_cursor_did_not_warp=True)

        time.sleep(ui.api.GetDoubleClickTime() / 1000 + 0.05)
        before_events, before_opens = target.snapshot()
        physical_double_click(client, canvas, p)
        wait_until(lambda: len(target.snapshot()[1]) > len(before_opens), args.timeout, "Native listbox double-click did not open its real temporary file")
        time.sleep(0.35)
        events, opens = target.snapshot()
        new_opens = opens[len(before_opens):]
        require(len(new_opens) == 1 and new_opens[0]["file"] == "alpha.txt", "One physical double-click opened the wrong file or opened more than once")
        require(target.displayed_text().replace("\r\n", "\n") == target.paths[0].read_text(encoding="utf-8"),
                "Opened file content was not displayed by the native target")
        record("double_click_reads_real_file_once", opened=new_opens[0], target_events=events[len(before_events):])

        time.sleep(ui.api.GetDoubleClickTime() / 1000 + 0.05)
        q = target.item_point(1)
        p = point_on_canvas(ui, canvas, q)
        ui.move_physical(client, p, canvas)
        cursor_stays(p)
        before = len(target.snapshot()[0])
        before_opens = len(target.snapshot()[1])
        require(target.selected_index() == 0, "Target selection changed before drag down")
        ui.button(client, canvas, True)
        wait_until(lambda: target.selected_index() == 1, args.timeout, "Native listbox selection did not change on mapped down")
        wait_until(lambda: ui.thread_capture(canvas) == canvas, args.timeout, "Client did not retain capture during the drag")
        require(ui.thread_capture(target.listbox) == 0, "Underlying listbox stole physical capture")
        ui.move_physical(client, outside)
        time.sleep(0.15)
        ui.button(client, canvas, False, require_canvas=False)
        wait_until(lambda: ui.thread_capture(canvas) == 0, args.timeout, "Client retained capture after release outside the canvas")
        events = target.snapshot()[0][before:]
        require(ui.thread_capture(target.listbox) == 0, "Native target retained capture after release")
        require(len(target.snapshot()[1]) == before_opens, "Dragging unexpectedly opened a file")
        require(not (ui.api.GetAsyncKeyState(1) & 0x8000), "Physical left button remained pressed")
        record("drag_outside_canvas_releases", target_events=events, selected_index=target.selected_index(),
               client_capture_after=ui.thread_capture(canvas), target_capture_after=ui.thread_capture(target.listbox),
               semantic_control_actions=True)

        ui.api.PostMessageW(main_window, 0x0112, 0xF020, 0)
        wait_until(lambda: ui.api.IsIconic(main_window), args.timeout, "Client did not minimize")
        ui.api.PostMessageW(main_window, 0x0112, 0xF120, 0)
        wait_until(lambda: not ui.api.IsIconic(main_window), args.timeout, "Client did not restore")
        ui.raise_own(client, main_window)
        ui.click(bar, FIT_ID)
        time.sleep(0.2)
        q = target.item_point(1)
        p = point_on_canvas(ui, canvas, q)
        before_opens = len(target.snapshot()[1])
        physical_double_click(client, canvas, p)
        wait_until(lambda: len(target.snapshot()[1]) > before_opens, args.timeout, "Restored Client could not open a real file")
        time.sleep(0.3)
        opens = target.snapshot()[1][before_opens:]
        require(len(opens) == 1 and opens[0]["file"] == "beta.txt", "Restored control duplicated/misdirected file open")
        require(target.displayed_text().replace("\r\n", "\n") == target.paths[1].read_text(encoding="utf-8"), "Second file was not displayed")
        record("control_after_minimize_restore", opened=opens[0])
        require(target.error is None, "Native file target failed: " + str(target.error))
        success = True
    except Exception as exc:
        result["error"] = {"type": type(exc).__name__, "message": str(exc)}
        if target:
            events, opens = target.snapshot()
            result["target_events_tail"] = events[-60:]
            result["opened_files"] = opens
        ui.api.GetForegroundWindow.restype = wintypes.HWND
        foreground = ui.api.GetForegroundWindow()
        result["failure_foreground"] = {"hwnd": int(foreground or 0), "pid": ui.pid(foreground) if foreground else 0}
        result["failure_clients"] = []
        for process in processes:
            if process.poll() is not None:
                continue
            for window in ui.windows(process.pid):
                display = ui.api.GetDlgItem(window, DISPLAY_ID)
                bar = ui.api.GetDlgItem(window, BAR_ID)
                if display and bar:
                    surface = ui.api.GetDlgItem(display, CANVAS_ID)
                    result["failure_clients"].append({"pid": process.pid, "hwnd": int(window), "title": ui.title(window),
                        "canvas": int(surface or 0), "thread_capture": ui.thread_capture(surface) if surface else 0})
    finally:
        ui.emergency_release()
        for process in reversed(processes):
            if process.poll() is None:
                ui.close_windows(process)
                try:
                    process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=3)
        # Guard is still present when restoring the pointer and destroying the
        # target. No cleanup button/key messages are sent to any user window.
        ui.api.SetCursorPos(*original_cursor)
        if target:
            target.close()
        if guard:
            guard.close()
        result["success"] = success
        result["own_processes"] = [{"pid": p.pid, "exit_code": p.returncode} for p in processes]
        result["data_retained"] = args.keep_data or not success
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(result, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
        if success and not args.keep_data:
            require(root.parent == Path(tempfile.gettempdir()).resolve() and root.name.startswith("rcs-local-control-"), "Unsafe cleanup target")
            shutil.rmtree(root)
        print(json.dumps(result, ensure_ascii=True), flush=True)
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
