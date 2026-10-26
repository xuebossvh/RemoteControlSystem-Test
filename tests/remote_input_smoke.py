"""Opt-in Windows loopback input regression.

Starts only an isolated Server and creates its own no-activate mouse target.
It sends NO keyboard or button events. Cursor movement is at most 10 pixels and
is restored while the target still covers the original location. Do not run
concurrently with another interactive UI test. No desktop images are saved.
"""
from __future__ import annotations
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
from runtime_smoke import WinUI, Peer, create_fixtures, wait_until, require, ADMIN_PASSWORD, MOUSE

MARKER = 0x52435331

class MouseTarget:
    def __init__(self, ui, original):
        self.u = ui.api
        self.events = []
        self.proc_type = c.WINFUNCTYPE(c.c_ssize_t, w.HWND, w.UINT, w.WPARAM, w.LPARAM)
        class WindowClass(c.Structure):
            _fields_ = [("style", w.UINT), ("proc", self.proc_type), ("class_extra", c.c_int),
                        ("window_extra", c.c_int), ("instance", w.HINSTANCE), ("icon", w.HICON),
                        ("cursor", w.HANDLE), ("background", w.HBRUSH), ("menu", w.LPCWSTR), ("name", w.LPCWSTR)]
        self.u.DefWindowProcW.argtypes = [w.HWND,w.UINT,w.WPARAM,w.LPARAM]
        self.u.DefWindowProcW.restype = c.c_ssize_t
        self.u.GetMessageExtraInfo.restype = c.c_ssize_t
        self.u.RegisterClassW.argtypes = [c.POINTER(WindowClass)]
        self.u.CreateWindowExW.argtypes = [w.DWORD,w.LPCWSTR,w.LPCWSTR,w.DWORD,c.c_int,c.c_int,c.c_int,c.c_int,w.HWND,w.HMENU,w.HINSTANCE,c.c_void_p]
        self.u.CreateWindowExW.restype = w.HWND
        self.u.ShowWindow.argtypes = [w.HWND,c.c_int]
        self.u.DestroyWindow.argtypes = [w.HWND]
        self.u.DispatchMessageW.argtypes = [c.POINTER(w.MSG)]
        self.u.DispatchMessageW.restype = c.c_ssize_t
        kernel = c.WinDLL("kernel32", use_last_error=True)
        kernel.GetModuleHandleW.argtypes = [w.LPCWSTR]
        kernel.GetModuleHandleW.restype = w.HMODULE
        instance = kernel.GetModuleHandleW(None)
        @self.proc_type
        def receive(window, message, wp, lp):
            if message == 0x0200:
                self.events.append({"extra_info": self.u.GetMessageExtraInfo() & 0xffffffff,
                                    "local": [c.c_short(lp & 0xffff).value, c.c_short((lp >> 16) & 0xffff).value]})
            return self.u.DefWindowProcW(window, message, wp, lp)
        self.callback = receive
        name = "RCSRemoteInputSmokeOwnWindow"
        wc = WindowClass(0, receive, 0, 0, instance, None, None, w.HBRUSH(6), None, name)
        require(bool(self.u.RegisterClassW(c.byref(wc))), "Register target window failed")
        width, height = self.u.GetSystemMetrics(0), self.u.GetSystemMetrics(1)
        x, y = min(max(0, original[0]-24), width-64), min(max(0, original[1]-24), height-64)
        self.window = self.u.CreateWindowExW(0x08000088, name, "RCS isolated mouse test", 0x80000000,
                                            x, y, 64, 64, None, None, instance, None)
        require(bool(self.window), "Create target window failed")
        self.u.ShowWindow(self.window, 4)
        self.pump(0.1)
        self.events.clear()
    def pump(self, duration):
        deadline = time.monotonic() + duration
        message = w.MSG()
        while time.monotonic() < deadline:
            while self.u.PeekMessageW(c.byref(message), None, 0, 0, 1):
                self.u.TranslateMessage(c.byref(message)); self.u.DispatchMessageW(c.byref(message))
            time.sleep(0.005)
    def close(self):
        if self.window: self.u.DestroyWindow(self.window); self.window = None

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    executable = (args.bin_dir / "Server.exe").resolve()
    require(executable.is_file(), "Server.exe not found")
    ui = WinUI()
    root = Path(tempfile.mkdtemp(prefix="rcs-input-smoke-")).resolve()
    process = None; target = None; original = None; success = False
    result = {"success": False, "keyboard_or_click_injected": False, "cases": []}
    try:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0)); port = probe.getsockname()[1]
        server_data, _ = create_fixtures(root, port)
        process = subprocess.Popen([str(executable), "--data-dir", str(server_data)], cwd=server_data)
        login = ui.dialog(process, (1017, 1), 12)
        ui.set_text(ui.api.GetDlgItem(login, 1017), ADMIN_PASSWORD); ui.click(login, 1)
        main_window = ui.dialog(process, (1000, 1007), 12); ui.click(main_window, 1007)
        def listening():
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.2): return True
            except OSError: return False
        wait_until(listening, 12, "Server listener did not start")
        original = ui.cursor()
        width, height = ui.api.GetSystemMetrics(0), ui.api.GetSystemMetrics(1)
        require(0 <= original[0] < width and 0 <= original[1] < height, "Move pointer to primary display before this opt-in test")
        destination = (original[0]+(8 if original[0]+8 < width else -8),
                       original[1]+(6 if original[1]+6 < height else -6))
        target = MouseTarget(ui, original)
        with Peer(port, 8) as viewer:
            require(viewer.login("view") == 0, "Viewer login failed")
            viewer.sock.sendall(struct.pack("<Biii", MOUSE, *destination, 0))
            target.pump(.25)
            require(ui.cursor() == original and not any(e["extra_info"] == MARKER for e in target.events),
                    "Viewing account injected input")
        result["cases"].append({"name": "viewer_cannot_move", "pass": True})
        target.events.clear()
        with Peer(port, 8) as controller:
            require(controller.login("control") == 1, "Controller login failed")
            controller.sock.sendall(struct.pack("<Biii", MOUSE, *destination, 0))
            deadline = time.monotonic()+2
            while time.monotonic() < deadline and not any(e["extra_info"] == MARKER for e in target.events): target.pump(.02)
            actual = ui.cursor()
            require(abs(actual[0]-destination[0]) <= 1 and abs(actual[1]-destination[1]) <= 1, "Physical cursor did not reach requested pixel")
            tagged = [e for e in target.events if e["extra_info"] == MARKER]
            require(tagged, "Target WM_MOUSEMOVE lost injection marker")
            result["cases"].append({"name": "control_move_has_marker", "pass": True,
                                    "requested": list(destination), "actual": list(actual), "tagged_events": len(tagged), "marker": MARKER})
        success = True
    except Exception as error:
        result["error"] = str(error)
    finally:
        if original and target:
            ui.api.SetCursorPos(*original); target.pump(.03); target.close()
        if process and process.poll() is None:
            ui.close_windows(process)
            try: process.wait(timeout=4)
            except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=4)
        result["success"] = success
        result["temporary_directory"] = str(root)
        if success:
            require(root.parent == Path(tempfile.gettempdir()).resolve() and root.name.startswith("rcs-input-smoke-"), "Unsafe cleanup path")
            shutil.rmtree(root)
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(result, indent=2)+"\n", encoding="utf-8")
        print(json.dumps(result), flush=True)
    return 0 if success else 1
if __name__ == "__main__": raise SystemExit(main())
