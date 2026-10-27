"""Opt-in Server mouse regression; run only in an exclusive UI test slot.

Starts an isolated Server and a private blank no-activate input target. Tests
movement, two-click sequences, middle button, both wheel axes and release on
disconnect. Every injected point must hit that exact target. Wheel cases
explicitly focus only this private target, so inactive-scroll policy cannot
route them to another application. No keyboard input is injected; no captured
image is saved. The original cursor is restored before the target is destroyed.
"""
from __future__ import annotations
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
from runtime_smoke import WinUI, Peer, create_fixtures, wait_until, require, ADMIN_PASSWORD, MOUSE

MARKER = 0x52435331
MOVE = 0x0200
LEFT_DOWN, LEFT_UP, LEFT_DOUBLE = 0x0201, 0x0202, 0x0203
RIGHT_DOWN, RIGHT_UP, RIGHT_DOUBLE = 0x0204, 0x0205, 0x0206
MIDDLE_DOWN, MIDDLE_UP, MIDDLE_DOUBLE = 0x0207, 0x0208, 0x0209
WHEEL, HWHEEL = 0x020A, 0x020E
BUTTONS = {LEFT_DOWN: (1, 0x0004), RIGHT_DOWN: (2, 0x0010), MIDDLE_DOWN: (4, 0x0040)}
MOUSE_MESSAGES = {MOVE, LEFT_DOWN, LEFT_UP, LEFT_DOUBLE, RIGHT_DOWN, RIGHT_UP, RIGHT_DOUBLE,
                  MIDDLE_DOWN, MIDDLE_UP, MIDDLE_DOUBLE, WHEEL, HWHEEL}


class MouseTarget:
    def __init__(self, ui, original):
        self.u = ui.api
        self.events = []
        self.owned_buttons = set()
        self.proc_type = c.WINFUNCTYPE(c.c_ssize_t, w.HWND, w.UINT, w.WPARAM, w.LPARAM)
        class WindowClass(c.Structure):
            _fields_ = [("style", w.UINT), ("proc", self.proc_type), ("class_extra", c.c_int),
                        ("window_extra", c.c_int), ("instance", w.HINSTANCE), ("icon", w.HICON),
                        ("cursor", w.HANDLE), ("background", w.HBRUSH), ("menu", w.LPCWSTR), ("name", w.LPCWSTR)]
        self.u.DefWindowProcW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
        self.u.DefWindowProcW.restype = c.c_ssize_t
        self.u.GetMessageExtraInfo.restype = c.c_ssize_t
        self.u.RegisterClassW.argtypes = [c.POINTER(WindowClass)]
        self.u.CreateWindowExW.argtypes = [w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD, c.c_int, c.c_int,
                                          c.c_int, c.c_int, w.HWND, w.HMENU, w.HINSTANCE, c.c_void_p]
        self.u.CreateWindowExW.restype = w.HWND
        self.u.ShowWindow.argtypes = [w.HWND, c.c_int]
        self.u.DestroyWindow.argtypes = [w.HWND]
        self.u.DispatchMessageW.argtypes = [c.POINTER(w.MSG)]
        self.u.DispatchMessageW.restype = c.c_ssize_t
        self.u.WindowFromPoint.argtypes = [w.POINT]
        self.u.WindowFromPoint.restype = w.HWND
        self.u.GetAsyncKeyState.argtypes = [c.c_int]
        self.u.GetAsyncKeyState.restype = c.c_short
        self.u.SetForegroundWindow.argtypes = [w.HWND]
        self.u.SetForegroundWindow.restype = w.BOOL
        self.u.GetForegroundWindow.restype = w.HWND
        self.u.SetFocus.argtypes = [w.HWND]
        self.u.SetFocus.restype = w.HWND
        self.u.GetFocus.restype = w.HWND
        kernel = c.WinDLL("kernel32", use_last_error=True)
        kernel.GetModuleHandleW.argtypes = [w.LPCWSTR]
        kernel.GetModuleHandleW.restype = w.HMODULE
        self.instance = kernel.GetModuleHandleW(None)

        @self.proc_type
        def receive(window, message, wp, lp):
            if message == 0x0021:  # WM_MOUSEACTIVATE: ordinary clicks never activate another app.
                return 3  # MA_NOACTIVATE, without discarding the click.
            if message in MOUSE_MESSAGES:
                self.events.append({"message": message, "extra_info": self.u.GetMessageExtraInfo() & 0xffffffff,
                                    "point": [c.c_short(lp & 0xffff).value, c.c_short((lp >> 16) & 0xffff).value],
                                    "delta": c.c_short((wp >> 16) & 0xffff).value if message in (WHEEL, HWHEEL) else 0})
                return 0  # Never open a context menu or forward scrolling to another window.
            return self.u.DefWindowProcW(window, message, wp, lp)

        self.callback = receive
        self.class_name = "RCSRemoteInputSmokeOwnWindow"
        wc = WindowClass(0x0008, receive, 0, 0, self.instance, None, None, w.HBRUSH(6), None, self.class_name)
        require(bool(self.u.RegisterClassW(c.byref(wc))), "Register target window failed")
        width, height = self.u.GetSystemMetrics(0), self.u.GetSystemMetrics(1)
        x, y = min(max(0, original[0] - 40), width - 96), min(max(0, original[1] - 40), height - 96)
        self.window = self.u.CreateWindowExW(0x08000088, self.class_name, "RCS isolated mouse test", 0x80000000,
                                            x, y, 96, 96, None, None, self.instance, None)
        require(bool(self.window), "Create target window failed")
        self.u.ShowWindow(self.window, 4)
        self.pump(0.1)
        self.events.clear()

    def require_point(self, point):
        actual = self.u.WindowFromPoint(w.POINT(*point))
        require(actual == self.window, "Input point is not on our exact private target; injection refused")
        process = w.DWORD()
        self.u.GetWindowThreadProcessId(actual, c.byref(process))
        require(process.value == os.getpid(), "Input target is not owned by this test")

    def pump(self, duration):
        deadline = time.monotonic() + duration
        message = w.MSG()
        while time.monotonic() < deadline:
            while self.u.PeekMessageW(c.byref(message), None, 0, 0, 1):
                self.u.TranslateMessage(c.byref(message))
                self.u.DispatchMessageW(c.byref(message))
            time.sleep(0.003)

    def wait(self, predicate, description, timeout=3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pump(0.015)
            if predicate():
                return
        raise TimeoutError(description)

    def tagged(self, messages=None):
        return [event for event in self.events if event["extra_info"] == MARKER and
                (messages is None or event["message"] in messages)]

    def focus_for_wheel(self):
        # Keep WS_EX_NOACTIVATE for ordinary clicks, but deliberately focus this
        # owned target for wheel injection. Otherwise a user's focus-only wheel
        # setting could deliver to an unrelated foreground application.
        self.u.SetForegroundWindow(self.window)
        self.pump(0.05)
        require(self.u.GetForegroundWindow() == self.window, "Cannot safely focus our wheel target")
        self.u.SetFocus(self.window)
        require(self.u.GetFocus() == self.window, "Wheel target lacks keyboard focus; no wheel injected")

    def buttons_released(self):
        return all(not (self.u.GetAsyncKeyState(key) & 0x8000) for key in (1, 2, 4))

    def emergency_release(self, point):
        flags = 0
        for down in self.owned_buttons:
            key, release = BUTTONS[down]
            if self.u.GetAsyncKeyState(key) & 0x8000:
                flags |= release
        if not flags:
            return False
        self.require_point(point)
        require(bool(self.u.SetCursorPos(*point)), "Cannot place cursor on our release target")
        self.require_point(point)
        class MouseInput(c.Structure):
            _fields_ = [("x", w.LONG), ("y", w.LONG), ("data", w.DWORD), ("flags", w.DWORD),
                        ("time", w.DWORD), ("extra", c.c_size_t)]
        class Input(c.Structure):
            _fields_ = [("kind", w.DWORD), ("mouse", MouseInput)]
        self.u.SendInput.argtypes = [w.UINT, c.POINTER(Input), c.c_int]
        self.u.SendInput.restype = w.UINT
        event = Input(0, MouseInput(0, 0, 0, flags, 0, MARKER))
        require(self.u.SendInput(1, c.byref(event), c.sizeof(event)) == 1, "Emergency button release failed")
        self.pump(0.05)
        return True

    def close(self):
        if self.window:
            self.u.DestroyWindow(self.window)
            self.window = None
        self.u.UnregisterClassW.argtypes = [w.LPCWSTR, w.HINSTANCE]
        self.u.UnregisterClassW(self.class_name, self.instance)


def wheel_packet(delta, horizontal=False):
    return c.c_int32(((delta & 0xffff) << 16) | (HWHEEL if horizontal else WHEEL)).value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    require(os.name == "nt", "An interactive Windows desktop is required")
    executable = (args.bin_dir / "Server.exe").resolve()
    require(executable.is_file(), "Server.exe not found")
    ui = WinUI()
    root = Path(tempfile.mkdtemp(prefix="rcs-input-smoke-")).resolve()
    process = target = controller = None
    original = destination = None
    result = {"success": False, "keyboard_injected": False, "clicks_only_in_own_target": True,
              "screenshots_saved": False, "cases": []}

    def record(name, **details):
        case = {"name": name, "pass": True, **details}
        result["cases"].append(case)
        print(json.dumps(case), flush=True)

    try:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        server_data, _ = create_fixtures(root, port)
        process = subprocess.Popen([str(executable), "--data-dir", str(server_data)], cwd=server_data)
        result["own_server_pid"] = process.pid
        login = ui.dialog(process, (1017, 1), 12)
        ui.set_text(ui.api.GetDlgItem(login, 1017), ADMIN_PASSWORD)
        ui.click(login, 1)
        main_window = ui.dialog(process, (1000, 1007), 12)
        ui.click(main_window, 1007)

        def listening():
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.2):
                    return True
            except OSError:
                return False
        wait_until(listening, 12, "Server listener did not start")
        original = ui.cursor()
        width, height = ui.api.GetSystemMetrics(0), ui.api.GetSystemMetrics(1)
        require(0 <= original[0] < width and 0 <= original[1] < height,
                "Move pointer to primary display before this opt-in test")
        destination = (original[0] + (8 if original[0] + 8 < width else -8),
                       original[1] + (6 if original[1] + 6 < height else -6))
        target = MouseTarget(ui, original)
        require(target.buttons_released(), "A physical mouse button is already down; no test input sent")
        target.require_point(original)
        target.require_point(destination)
        with Peer(port, 8) as viewer:
            require(viewer.login("view") == 0, "Viewer login failed")
            viewer.sock.sendall(struct.pack("<Biii", MOUSE, *destination, 0))
            target.pump(.25)
            require(ui.cursor() == original and not target.tagged(), "Viewing account injected input")
        record("viewer_cannot_move")

        controller = Peer(port, 8)
        require(controller.login("control") == 1, "Controller login failed")

        def send(status):
            target.require_point(destination)
            require(process.poll() is None, "Our Server exited unexpectedly")
            if status in BUTTONS:
                target.owned_buttons.add(status)
            controller.sock.sendall(struct.pack("<Biii", MOUSE, *destination, status))

        target.events.clear()
        send(0)
        target.wait(lambda: bool(target.tagged({MOVE})), "Marked mouse move was not delivered")
        actual = ui.cursor()
        require(max(abs(a - b) for a, b in zip(actual, destination)) <= 1, "Mouse did not reach requested pixel")
        record("control_move_has_marker", requested=list(destination), actual=list(actual), marker=MARKER)

        for name, down, up, double in (("left", LEFT_DOWN, LEFT_UP, LEFT_DOUBLE),
                                       ("right", RIGHT_DOWN, RIGHT_UP, RIGHT_DOUBLE)):
            target.events.clear()
            for status in (down, up, double, up):
                send(status)
                target.pump(.045)
            target.wait(lambda: len(target.tagged({down, up, double})) >= 4, "Two-click sequence was not delivered")
            target.pump(.15)  # Detect an erroneous third click after the expected events.
            events = target.tagged({down, up, double})
            downs = sum(event["message"] in (down, double) for event in events)
            ups = sum(event["message"] == up for event in events)
            require(downs == 2 and ups == 2 and len(events) == 4, "Double click injected an extra/missing down/up pair")
            require(target.buttons_released(), "Double click left a physical button held")
            record(name + "_double_click_is_two_pairs", downs=downs, ups=ups,
                   messages=[event["message"] for event in events])

        target.events.clear()
        send(MIDDLE_DOWN)
        target.pump(.045)
        send(MIDDLE_UP)
        target.wait(lambda: len(target.tagged({MIDDLE_DOWN, MIDDLE_DOUBLE, MIDDLE_UP})) >= 2, "Middle button was not delivered")
        events = target.tagged({MIDDLE_DOWN, MIDDLE_DOUBLE, MIDDLE_UP})
        require(sum(event["message"] in (MIDDLE_DOWN, MIDDLE_DOUBLE) for event in events) == 1 and
                sum(event["message"] == MIDDLE_UP for event in events) == 1 and target.buttons_released(),
                "Middle button down/up mismatch")
        record("middle_button_pair")

        target.focus_for_wheel()
        for horizontal, message, name in ((False, WHEEL, "vertical"), (True, HWHEEL, "horizontal")):
            target.events.clear()
            for delta in (120, -120):
                require(target.u.GetForegroundWindow() == target.window and target.u.GetFocus() == target.window,
                        "Wheel focus changed; no input sent")
                send(wheel_packet(delta, horizontal))
                target.pump(.06)
            target.wait(lambda: len(target.tagged({message})) >= 2, name + " wheel was not delivered")
            deltas = [event["delta"] for event in target.tagged({message})]
            require(deltas == [120, -120], name + " wheel delta/order mismatch")
            record(name + "_wheel_signed_deltas", deltas=deltas, target_explicitly_focused=True)

        target.events.clear()
        send(LEFT_DOWN)
        send(MIDDLE_DOWN)
        target.wait(lambda: len(target.tagged({LEFT_DOWN, LEFT_DOUBLE, MIDDLE_DOWN, MIDDLE_DOUBLE})) >= 2,
                    "Held buttons were not delivered")
        target.require_point(ui.cursor())
        controller.close()
        controller = None
        target.wait(lambda: len(target.tagged({LEFT_UP, MIDDLE_UP})) >= 2 and target.buttons_released(),
                    "Disconnect did not release all injected buttons")
        require(len(target.tagged({LEFT_UP})) == 1 and len(target.tagged({MIDDLE_UP})) == 1,
                "Disconnect release was missing or duplicated")
        record("disconnect_releases_held_buttons", released_buttons=["left", "middle"], marker=MARKER)
        result["success"] = True
    except Exception as error:
        result["error"] = str(error)
    finally:
        if controller:
            controller.close()
        if target:
            try:
                target.pump(.3)
                if destination and target.emergency_release(destination):
                    result["emergency_button_release"] = True
                    result["success"] = False
                if original:
                    ui.api.SetCursorPos(*original)
                    target.pump(.03)
                    result["cursor_restored"] = ui.cursor() == original
                    require(result["cursor_restored"], "Original cursor position was not restored")
            except Exception as error:
                result["success"] = False
                result["input_cleanup_error"] = str(error)
            finally:
                target.close()
        if process and process.poll() is None:
            ui.close_windows(process)
            try:
                process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=4)
                result["forced_own_server_cleanup"] = True
                result["success"] = False
        result["server_exit"] = process.returncode if process else None
        result["temporary_directory"] = str(root)
        if result["success"]:
            require(not root.is_symlink() and root.parent == Path(tempfile.gettempdir()).resolve() and
                    root.name.startswith("rcs-input-smoke-"), "Unsafe cleanup path")
            shutil.rmtree(root)
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(result), flush=True)
    return 0 if result["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
