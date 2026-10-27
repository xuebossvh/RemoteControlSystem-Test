"""Exclusive opt-in real Explorer + real Client/Server same-desktop regression.

Creates one unique temporary .cmd whose only action appends an 'opened' marker
inside that same directory. Real mouse input goes only to this run's Client.
Explorer is identified by an exact new HWND AND exact temporary folder path;
only that window is closed. No Explorer process is killed, no file association
is changed, and no screenshots are saved. Never run beside another GUI test.
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
import subprocess
import tempfile
import time
import uuid
import winreg
from runtime_smoke import ADMIN_PASSWORD, PASSWORD, create_fixtures, require, wait_until
from local_control_smoke import LocalInputUI, point_on_canvas
from capture_exclusion_smoke import SyntheticWindow
from ui_modes_smoke import DISPLAY_ID, CANVAS_ID, BAR_ID, FIT_ID

SHELL_HELPER = r'''param([string]$Mode, [string]$Folder, [long]$Window = 0, [string]$ItemName = '')
$ErrorActionPreference = 'Stop'
$shell = New-Object -ComObject Shell.Application
$entries = @()
foreach ($entry in $shell.Windows()) {
    try {
        if ([IO.Path]::GetFileName([string]$entry.FullName) -ine 'explorer.exe') { continue }
        if ($Mode -eq 'snapshot') { $entries += [long]$entry.HWND; continue }
        $path = [string]$entry.Document.Folder.Self.Path
        if (![string]::Equals($path, $Folder, [StringComparison]::OrdinalIgnoreCase)) { continue }
        if ($Window -and [long]$entry.HWND -ne $Window) { continue }
        if ($Mode -eq 'close') { $entry.Quit(); $entries += @{ closed = $true; hwnd = $Window }; break }
        if ($Mode -eq 'match') { $entries += @{ hwnd = [long]$entry.HWND; location = [string]$entry.LocationURL }; continue }
        if ($Mode -eq 'item') {
            Add-Type -AssemblyName UIAutomationClient
            Add-Type -AssemblyName UIAutomationTypes
            $root = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$Window)
            $nodes = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                                  [System.Windows.Automation.Condition]::TrueCondition)
            foreach ($node in $nodes) {
                $name = $node.Current.Name
                if ($name -ine $ItemName -and $name -ine [IO.Path]::GetFileNameWithoutExtension($ItemName)) { continue }
                $box = $node.Current.BoundingRectangle
                if ($box.IsEmpty -or $box.Width -le 0 -or $box.Height -le 0) { continue }
                $entries += @{ name = $name; type = $node.Current.ControlType.ProgrammaticName;
                               left = $box.Left; top = $box.Top; width = $box.Width; height = $box.Height }
            }
        }
    } catch { if ($Mode -eq 'item' -or $Mode -eq 'close') { throw } }
}
ConvertTo-Json -InputObject @($entries) -Compress -Depth 5
'''

RUN_KEY = r'Software\Microsoft\Windows\CurrentVersion\Run'
def save_autorun():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            return winreg.QueryValueEx(key, 'RemoteControlServer')
    except FileNotFoundError:
        return None

def restore_autorun(saved):
    if saved is not None:
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            winreg.SetValueEx(key, 'RemoteControlServer', 0, saved[1], saved[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, required=True)
    parser.add_argument('--allow-input', action='store_true')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--keep-data', action='store_true')
    args = parser.parse_args()
    if not args.allow_input:
        parser.error('Run only in an exclusive desktop slot and pass --allow-input')
    binaries = args.bin_dir.resolve()
    require(all((binaries / file).is_file() for file in ('Client.exe', 'Server.exe')), 'Missing executables')
    ui = LocalInputUI()
    ui.api.IsWindow.argtypes = [wintypes.HWND]; ui.api.IsWindow.restype = wintypes.BOOL
    root = Path(tempfile.mkdtemp(prefix='rcs-explorer-control-')).resolve()
    helper = root / 'own-explorer.ps1'; helper.write_text(SHELL_HELPER, encoding='utf-8-sig')
    folder = root / 'files'; folder.mkdir()
    fixture = folder / ('rcs-proof-' + uuid.uuid4().hex[:12] + '.cmd')
    fixture.write_text('@echo off\n>> "%~dp0opened.marker" echo opened\nexit /b 0\n', encoding='ascii')
    marker = folder / 'opened.marker'
    original_cursor = ui.cursor(); original_autorun = save_autorun()
    processes = []; guard = None; explorer = None; before = set(); success = False
    client = None; main_client = None; phase = "setup"
    report = {'success': False, 'real_explorer': True, 'real_client_and_server': True,
              'temporary_directory': str(root), 'original_cursor': list(original_cursor), 'file_associations_changed': False,
              'target_input_messages_sent_by_test': 0, 'screenshots_saved': False, 'cases': []}

    def record(name, **details):
        item = {'name': name, 'status': 'pass', 'details': details}; report['cases'].append(item)
        print(json.dumps(item, ensure_ascii=True), flush=True)

    def shell(mode, window=0):
        result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                                 '-File', str(helper), '-Mode', mode, '-Folder', str(folder), '-Window', str(window),
                                 '-ItemName', fixture.name], capture_output=True, timeout=20, creationflags=subprocess.CREATE_NO_WINDOW)
        require(result.returncode == 0, 'Explorer query failed: ' + result.stderr.decode('utf-8', errors='replace'))
        text = result.stdout.decode('utf-8-sig', errors='replace').strip()
        return json.loads(text) if text else []

    def launch(name, data):
        process = subprocess.Popen([str(binaries/name), '--data-dir', str(data)], cwd=data)
        processes.append(process); return process

    def cursor_stays(point):
        time.sleep(.15)
        require(max(abs(a-b) for a,b in zip(ui.cursor(), point)) <= 1, 'Same-desktop control warped the pointer')

    try:
        before = set(shell('snapshot'))
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0)); port = reservation.getsockname()[1]
        server_data, client_data = create_fixtures(root, port)
        server = launch('Server.exe', server_data)
        auth = ui.dialog(server, (1017,1), 15)
        ui.set_text(ui.api.GetDlgItem(auth,1017), ADMIN_PASSWORD); ui.click(auth,1)
        main_server = ui.dialog(server, (1000,1007),15); ui.click(main_server,1007)
        def listening():
            try:
                with socket.create_connection(('127.0.0.1',port),timeout=.2): return True
            except OSError: return False
        wait_until(listening,15,'Isolated Server did not listen')
        guard = SyntheticWindow(ui,(0,0,ui.width,ui.height))
        # /n asks Explorer for a new window; verify identity rather than trusting it.
        subprocess.Popen(['explorer.exe', '/n,/e,' + str(folder)])
        def find_new():
            return next((int(item['hwnd']) for item in shell('match') if int(item['hwnd']) not in before), None)
        explorer = wait_until(find_new,25,'No new Explorer HWND for the exact fixture folder')
        require(explorer not in before and ui.api.IsWindow(explorer), 'Explorer ownership validation failed')
        x,y = ui.width//6+80, ui.height//6+120
        require(bool(ui.api.SetWindowPos(explorer, ctypes.c_void_p(-1), x,y,900,700,0x0010|0x0040)), 'Position own Explorer window failed')
        items = wait_until(lambda: shell('item', explorer), 20, 'Explorer did not expose our file item')
        items = [item for item in items if item['type'] in ('ControlType.ListItem','ControlType.DataItem')]
        require(len(items)==1,'Expected exactly one Explorer file item, got '+repr(items))
        item=items[0]
        q=(round(item['left']+min(item['width']/2,60)),round(item['top']+item['height']/2))
        record('new_explorer_exact_folder', hwnd=explorer, pid=ui.pid(explorer), item=item, target_point=list(q), fixture_name=fixture.name)

        client=launch('Client.exe',client_data)
        login=ui.dialog(client,(1000,1012,1003,1004),15)
        ui.raise_own(client,login);ui.send(ui.api.GetDlgItem(login,1000),0x014E,1)
        ui.set_text(ui.api.GetDlgItem(login,1012),'control');ui.set_text(ui.api.GetDlgItem(login,1003),PASSWORD);ui.click(login,1004)
        main_client=ui.dialog(client,(DISPLAY_ID,BAR_ID),15)
        ui.place_own(client,main_client,ui.width//6,ui.height//6,ui.width*2//3,ui.height*2//3)
        canvas=ui.api.GetDlgItem(ui.api.GetDlgItem(main_client,DISPLAY_ID),CANVAS_ID)
        bar=ui.api.GetDlgItem(main_client,BAR_ID);ui.click(bar,FIT_ID)
        mode=ui.api.GetDlgItem(bar,4105)
        wait_until(lambda: '\u540c\u673a\u63a7\u5236' in ui.title(mode),15,'Verified local control mode was not selected')
        wait_until(lambda: ui.paint_count(canvas)>0,15,'Server images were not painted')
        time.sleep(.4)
        p=point_on_canvas(ui,canvas,q)
        ui.hit_is_client(client,q);ui.hit_is_client(client,p,canvas)
        require(abs(p[0]-q[0])+abs(p[1]-q[1])>30,'Mapped Q must differ from physical P')
        require(not marker.exists(),'Fixture marker exists before any click')
        ui.move_physical(client,p,canvas);cursor_stays(p)
        # First click selects the real shell item but must not execute it.
        phase='single_down'; ui.button(client,canvas,True);time.sleep(.035)
        phase='single_up'; ui.button(client,canvas,False)
        time.sleep(ui.api.GetDoubleClickTime()/1000+.15)
        require(not marker.exists(),'A single selection unexpectedly executed the file')
        cursor_stays(p)
        record('single_click_selects_without_opening',physical_point=list(p),covered_target_point=list(q))
        # Real hardware-style events go only to Client; Explorer is never clicked
        # or invoked by this test helper. The product must perform the opening.
        for click in range(2):
            phase=f'double_{click+1}_down'; ui.button(client,canvas,True);time.sleep(.035)
            phase=f'double_{click+1}_up'; ui.button(client,canvas,False);time.sleep(.035)
        wait_until(marker.is_file,12,'Explorer default action did not execute the harmless marker fixture')
        time.sleep(.3)
        lines=marker.read_text(encoding='ascii').splitlines()
        require(lines==['opened'],'One double-click invoked the Explorer file more than once')
        cursor_stays(p)
        record('real_explorer_double_click_opens_once',marker_lines=len(lines),physical_cursor_did_not_warp=True,
               fixture_only_writes_own_marker=True)
        success=True
    except Exception as error:
        report['error']={'type':type(error).__name__,'message':str(error),'phase':phase}
        position=ui.cursor(); hit=ui.api.WindowFromPoint(wintypes.POINT(*position))
        ui.api.GetForegroundWindow.restype=wintypes.HWND
        foreground=ui.api.GetForegroundWindow()
        report['failure_state']={'cursor':list(position),'hit_hwnd':int(hit or 0),'hit_pid':ui.pid(hit) if hit else 0,
                                 'foreground_hwnd':int(foreground or 0),'foreground_pid':ui.pid(foreground) if foreground else 0,
                                 'client_pid':client.pid if client else None,'explorer_hwnd':explorer,
                                 'marker_exists':marker.exists()}
        if client:
            report['failure_state']['client_windows']=[{'hwnd':int(w),'title':ui.title(w)} for w in ui.windows(client.pid)]
    finally:
        ui.emergency_release()
        # Cleanup-only focus restoration: never raise Client between the input
        # events under test, which would conceal a real proxy activation bug.
        restored = False
        if client and client.poll() is None and main_client and ui.api.IsWindow(main_client):
            try:
                ui.raise_own(client, main_client)
                restored = bool(ui.api.SetCursorPos(*original_cursor))
                time.sleep(.03)
            except Exception:
                pass
        report['restore_call']={'success':restored,'error':ctypes.get_last_error() if not restored else 0}
        report['cursor_after_restore']=list(ui.cursor())
        report['cursor_restored']=ui.cursor()==original_cursor
        for process in reversed(processes):
            if process.poll() is None:
                ui.close_windows(process)
                try:process.wait(timeout=4)
                except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=3)
        if explorer:
            try:
                require(explorer not in before,'Refusing to close an existing Explorer window')
                closed=shell('close',explorer)
                require(closed and closed[0].get('closed'),'Own Explorer folder identity changed; window left untouched')
                wait_until(lambda:not ui.api.IsWindow(explorer),5,'Own Explorer window did not close')
                report['own_explorer_closed']=True
            except Exception as cleanup_error:
                success=False;report['explorer_cleanup_error']=str(cleanup_error)
        if guard:guard.close()
        restore_autorun(original_autorun)
        report['own_processes']=[{'pid':p.pid,'exit_code':p.returncode} for p in processes]
        if not report.get('cursor_restored'):
            success=False; report['restore_error']='Original physical cursor position was not restored'
        report['success']=success
        report['data_retained']=args.keep_data or not success
        if args.report:
            args.report.parent.mkdir(parents=True,exist_ok=True)
            args.report.write_text(json.dumps(report,indent=2,ensure_ascii=True)+'\n',encoding='utf-8')
        if success and not args.keep_data:
            require(root.parent==Path(tempfile.gettempdir()).resolve() and root.name.startswith('rcs-explorer-control-'),'Unsafe cleanup path')
            shutil.rmtree(root)
        print(json.dumps(report,ensure_ascii=True),flush=True)
    return 0 if success else 1

if __name__=='__main__':raise SystemExit(main())
