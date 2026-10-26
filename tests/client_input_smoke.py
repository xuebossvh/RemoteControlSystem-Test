"""Opt-in Client mouse routing and injected-input feedback regression.

Run only in an exclusive interactive test slot:
  python tests/client_input_smoke.py --bin-dir build-v1.1.0/bin/Release

Starts a new Client with isolated data and a localhost protocol fixture that
returns control permission and one synthetic JPEG. No real Server is started,
no desktop is captured, and no keyboard input is injected. Mouse movement and
one left click target only our own Client canvas / Fit button, with HWND/PID
checks immediately before input. The original cursor position is restored
after our Client closes. Do not run alongside another interactive UI test.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
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

from runtime_smoke import PASSWORD, create_fixtures, require, wait_until
from ui_modes_smoke import BAR_ID, CANVAS_ID, DISPLAY_ID, FIT_ID, ModeUI, StaticFrameServer


MARKER = 0x52435331


class InputFixture(StaticFrameServer):
    """Receive real Client packets, without ever injecting anything remotely."""

    def __init__(self):
        self.packet_lock = threading.Lock()
        self.mouse_packets = []
        self.keyboard_packets = 0
        self.setup_packets = 0
        super().__init__(width=1600, height=1000)

    def snapshot(self):
        with self.packet_lock:
            return list(self.mouse_packets)

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
                require(kind == 4 and name.rstrip(b"\0") == b"control" and
                        password.rstrip(b"\0") == PASSWORD.encode("ascii"), "Unexpected fixture login")
                connection.sendall(struct.pack("<Biii", 5, 0x5100, 0x5110, 1))
                connection.sendall(struct.pack("<Biii", 5, 0x5200, self.width, self.height))
                connection.sendall(struct.pack("<BiiiiI", 1, 0, 0, self.width, self.height, len(self.jpeg)) + self.jpeg)
                self.images_sent = 1
                connection.settimeout(0.1)
                pending = bytearray()
                while not self.stop.is_set():
                    try:
                        part = connection.recv(4096)
                    except socket.timeout:
                        continue
                    if not part:
                        break
                    pending.extend(part)
                    while pending:
                        kind = pending[0]
                        length = {2: 13, 3: 9, 5: 13}.get(kind)
                        require(length is not None, "Unexpected Client packet type")
                        if len(pending) < length:
                            break
                        packet = bytes(pending[:length])
                        del pending[:length]
                        with self.packet_lock:
                            if kind == 2:
                                _, x, y, status = struct.unpack("<Biii", packet)
                                self.mouse_packets.append({"x": x, "y": y, "status": status})
                            elif kind == 3:
                                # Do not retain keyboard payloads, even on an unexpected event.
                                self.keyboard_packets += 1
                            else:
                                self.setup_packets += 1
        except Exception as error:
            if not self.stop.is_set():
                self.error = str(error)
        finally:
            self.disconnected.set()


class MouseInput(ctypes.Structure):
    _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                ("flags", wintypes.DWORD), ("time", wintypes.DWORD), ("extra", ctypes.c_size_t)]


class Input(ctypes.Structure):
    # MOUSEINPUT is the largest INPUT union member on both Win32 and Win64.
    _fields_ = [("type", wintypes.DWORD), ("mouse", MouseInput)]


class InputUI(ModeUI):
    def __init__(self):
        super().__init__()
        self.api.WindowFromPoint.argtypes = [wintypes.POINT]
        self.api.WindowFromPoint.restype = wintypes.HWND
        self.api.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(Input), ctypes.c_int]
        self.api.SendInput.restype = wintypes.UINT
        require(ctypes.sizeof(Input) == (40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28), "Unexpected INPUT ABI")

    def require_target(self, process, window, point):
        require(process.poll() is None and self.pid(window) == process.pid, "Input target is not our live Client")
        require(self.api.IsWindowVisible(window), "Our input target is hidden")
        actual = self.api.WindowFromPoint(wintypes.POINT(*point))
        require(actual == window and self.pid(actual) == process.pid,
                "Input point is not on the exact owned canvas/button; refusing global input")

    def move(self, process, window, point, marker=0):
        self.require_target(process, window, point)
        left, top = self.api.GetSystemMetrics(76), self.api.GetSystemMetrics(77)
        width, height = self.api.GetSystemMetrics(78), self.api.GetSystemMetrics(79)
        require(width > 0 and height > 0 and left <= point[0] < left + width and top <= point[1] < top + height,
                "Input point is outside the virtual desktop")
        # Target the centre of the requested physical pixel, avoiding endpoint rounding.
        x = min(65535, ((point[0] - left) * 65536 + 32768) // width)
        y = min(65535, ((point[1] - top) * 65536 + 32768) // height)
        event = Input(0, MouseInput(x, y, 0, 0x0001 | 0x8000 | 0x4000 | 0x2000, 0, marker))
        require(self.api.SendInput(1, ctypes.byref(event), ctypes.sizeof(event)) == 1, "SendInput mouse move failed")

    def click_own_button(self, process, button):
        point = self.cursor()
        self.require_target(process, button, point)
        # Submit down+up as one batch so a failure cannot leave a button held.
        events = (Input * 2)(Input(0, MouseInput(0, 0, 0, 0x0002, 0, 0)),
                             Input(0, MouseInput(0, 0, 0, 0x0004, 0, 0)))
        inserted = self.api.SendInput(2, events, ctypes.sizeof(Input))
        if inserted == 1:
            release = Input(0, MouseInput(0, 0, 0, 0x0004, 0, 0))
            self.api.SendInput(1, ctypes.byref(release), ctypes.sizeof(Input))
        require(inserted == 2, "SendInput did not complete our button click")


def expected_position(ui, canvas, fixture, screen_point):
    left, top, right, bottom = ui.screen_client_rect(canvas)
    width, height = right - left, bottom - top
    scale = min(width / fixture.width, height / fixture.height)
    image_width = max(1, math.floor(fixture.width * scale + 0.5))
    image_height = max(1, math.floor(fixture.height * scale + 0.5))
    x = screen_point[0] - left - (width - image_width) // 2
    y = screen_point[1] - top - (height - image_height) // 2
    require(0 <= x < image_width and 0 <= y < image_height, "Test point landed in a black bar")
    return (x * (fixture.width - 1) // (image_width - 1) if image_width > 1 else (fixture.width - 1) // 2,
            y * (fixture.height - 1) // (image_height - 1) if image_height > 1 else (fixture.height - 1) // 2)


def quiet_packets(fixture, timeout=2.0):
    deadline, stable_since, previous = time.monotonic() + timeout, time.monotonic(), len(fixture.snapshot())
    while time.monotonic() < deadline:
        require(fixture.error is None, "Fixture failed: " + str(fixture.error))
        count = len(fixture.snapshot())
        if count != previous:
            previous, stable_since = count, time.monotonic()
        if time.monotonic() - stable_since >= 0.25:
            return count
        time.sleep(0.02)
    raise TimeoutError("Mouse stream did not become quiet; another input source may be active")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--keep-data", action="store_true")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("An interactive Windows desktop is required")
    require(1 <= args.timeout <= 120, "Timeout must be between 1 and 120 seconds")
    executable = (args.bin_dir / "Client.exe").resolve()
    require(executable.is_file(), "Client.exe was not found")
    ui = InputUI()
    original_cursor = ui.cursor()
    root = Path(tempfile.mkdtemp(prefix="rcs-client-input-")).resolve()
    process = fixture = None
    cursor_changed = False
    report = {"success": False, "keyboard_injected": False, "real_server_started": False,
              "screenshots_saved": False, "cases": [], "temporary_directory": str(root)}

    def record(name, **details):
        entry = {"name": name, "pass": True, **details}
        report["cases"].append(entry)
        print(json.dumps(entry), flush=True)

    try:
        fixture = InputFixture()
        _, data = create_fixtures(root, fixture.port)
        process = subprocess.Popen([str(executable), "--data-dir", str(data)], cwd=data)
        report["own_client_pid"] = process.pid
        login = ui.dialog(process, (1000, 1012, 1003, 1004), args.timeout)
        ui.raise_own(process, login)
        require(ui.send(ui.api.GetDlgItem(login, 1000), 0x014E, 1) != ctypes.c_size_t(-1).value,
                "Synthetic host is missing")
        ui.set_text(ui.api.GetDlgItem(login, 1012), "control")
        ui.set_text(ui.api.GetDlgItem(login, 1003), PASSWORD)
        ui.click(login, 1004)
        main_window = ui.dialog(process, (DISPLAY_ID, BAR_ID), args.timeout, expected_title="Remote Control Client")
        _, work = ui.monitor_rectangles(main_window)
        width, height = min(900, work[2] - work[0]), min(650, work[3] - work[1])
        left = min(max(work[0], original_cursor[0] - width // 2), work[2] - width)
        top = min(max(work[1], original_cursor[1] - height // 2), work[3] - height)
        ui.place_own(process, main_window, left, top, width, height)
        display = ui.api.GetDlgItem(main_window, DISPLAY_ID)
        canvas = ui.api.GetDlgItem(display, CANVAS_ID)
        bar = ui.api.GetDlgItem(main_window, BAR_ID)
        button = ui.api.GetDlgItem(bar, FIT_ID)
        require(canvas and button and ui.pid(canvas) == process.pid and ui.pid(button) == process.pid,
                "Own Client canvas/Fit button is missing")
        wait_until(lambda: fixture.images_sent == 1 and ui.paint_count(canvas) > 0, args.timeout,
                   "Synthetic frame was not painted")
        ui.click(bar, FIT_ID)
        time.sleep(0.15)
        x0, y0, x1, y1 = ui.screen_client_rect(canvas)
        require(x1 - x0 >= 160 and y1 - y0 >= 100, "Canvas too small for safe interior movements")
        anchor = (min(max(original_cursor[0], x0 + (x1 - x0) // 3), x0 + 2 * (x1 - x0) // 3),
                  min(max(original_cursor[1], y0 + (y1 - y0) // 3), y0 + 2 * (y1 - y0) // 3))
        expected_position(ui, canvas, fixture, anchor)
        cursor_changed = True
        ui.move(process, canvas, anchor, MARKER)
        time.sleep(0.2)
        quiet_packets(fixture)

        def ordinary_move(point, name):
            before = quiet_packets(fixture)
            ui.move(process, canvas, point)
            wait_until(lambda: max(abs(a - b) for a, b in zip(ui.cursor(), point)) <= 1,
                       2, "Cursor did not reach our canvas point")
            actual = ui.cursor()
            expected = expected_position(ui, canvas, fixture, actual)

            def matching_packet():
                packets = fixture.snapshot()[before:]
                return next((packet for packet in reversed(packets) if packet["status"] == 0 and
                             abs(packet["x"] - expected[0]) <= 2 and abs(packet["y"] - expected[1]) <= 2), None)

            packet = wait_until(matching_packet, args.timeout, "Client did not send the correctly mapped MouseMsg")
            record(name, canvas_point=list(actual), expected_remote=list(expected), packet=packet)

        first = anchor[0] + 8, anchor[1] + 6
        ordinary_move(first, "unmarked_canvas_move_sends_mapped_mouse")
        before = quiet_packets(fixture)
        marked = first[0] + 7, first[1] + 5
        ui.move(process, canvas, marked, MARKER)
        wait_until(lambda: max(abs(a - b) for a, b in zip(ui.cursor(), marked)) <= 1,
                   2, "Marked cursor movement was not delivered")
        time.sleep(0.4)
        require(len(fixture.snapshot()) == before, "Server-tagged injected motion was echoed back by Client")
        record("marked_canvas_move_not_echoed", marker=MARKER, remote_mouse_packets=0)
        ordinary_move((marked[0] - 6, marked[1] - 4), "ordinary_move_still_works_after_marker")

        before = quiet_packets(fixture)
        bounds = ui.rect(button)
        button_point = ((bounds[0] + bounds[2]) // 2, (bounds[1] + bounds[3]) // 2)
        ui.move(process, button, button_point)
        wait_until(lambda: max(abs(a - b) for a, b in zip(ui.cursor(), button_point)) <= 1,
                   2, "Cursor did not reach our Fit button")
        time.sleep(0.1)
        ui.click_own_button(process, button)
        time.sleep(0.4)
        require(len(fixture.snapshot()) == before, "Toolbar hover/click was forwarded as remote mouse input")
        require(fixture.keyboard_packets == 0, "Unexpected keyboard packet during mouse-only regression")
        require(fixture.error is None and process.poll() is None, "Client or fixture failed")
        record("own_fit_button_click_not_forwarded", remote_mouse_packets=0, keyboard_packets=0)
        ui.api.PostMessageW(main_window, 0x0010, 0, 0)
        exit_code = process.wait(timeout=args.timeout)
        require(exit_code in (0, 2), f"Client did not exit normally: {exit_code}")
        require(fixture.disconnected.wait(3), "Fixture connection remained open")
        record("normal_client_cleanup", exit_code=exit_code, images_sent=fixture.images_sent)
        report["success"] = True
    except Exception as error:
        report["error"] = str(error)
    finally:
        try:
            if process and process.poll() is None:
                ui.close_windows(process)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=3)
                    report["forced_own_client_cleanup"] = True
        except Exception as error:
            report["success"] = False
            report["process_cleanup_error"] = str(error)
        try:
            if fixture:
                fixture.close()
        except Exception as error:
            report["success"] = False
            report["fixture_cleanup_error"] = str(error)
        if cursor_changed:
            # Close our controller first: restoration cannot send a remote command.
            try:
                ui.api.SetCursorPos(*original_cursor)
                report["cursor_restored"] = ui.cursor() == original_cursor
                require(report["cursor_restored"], "Original cursor position could not be restored")
            except Exception as error:
                report["success"] = False
                report["cursor_restore_error"] = str(error)
        if report["success"] and not args.keep_data:
            require(not root.is_symlink() and root.parent == Path(tempfile.gettempdir()).resolve() and
                    root.name.startswith("rcs-client-input-"), "Unsafe temporary cleanup path")
            shutil.rmtree(root)
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report), flush=True)
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
