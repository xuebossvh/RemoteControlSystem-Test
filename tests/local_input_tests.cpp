#include "../RemoteCtrolSystem/Common/LocalDesktopInput.h"
#include <windows.h>
#include <atomic>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<int> selections(0), doubleClicks(0), physicalMouseDowns(0), hitTests(0);
WNDPROC originalList = nullptr;
constexpr UINT kFinished = WM_APP + 73;
LRESULT CALLBACK ListProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) ++physicalMouseDowns;
    return CallWindowProcW(originalList, window, message, wp, lp);
}
LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCHITTEST) {
        ++hitTests;
        // A responsive GUI can legitimately spend 20 ms handling this request.
        // The prior 10 ms product deadline dropped an otherwise valid click.
        Sleep(20);
    } else if (message == WM_COMMAND && LOWORD(wp) == 6101) {
        if (HIWORD(wp) == LBN_SELCHANGE) ++selections;
        if (HIWORD(wp) == LBN_DBLCLK) ++doubleClicks;
        return 0;
    } else if (message == kFinished) {
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
void Check(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
}

int main() {
    SetProcessDPIAware();
    POINT originalCursor = {};
    GetCursorPos(&originalCursor);
    WNDCLASSW klass = {};
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpfnWndProc = WindowProcedure;
    klass.lpszClassName = L"RcsLocalProxyDelayedHitTest";
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&klass)) return 2;
    const int left = (GetSystemMetrics(SM_CXSCREEN) > 450) ? GetSystemMetrics(SM_CXSCREEN) - 420 : 0;
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        klass.lpszClassName, L"RCS local proxy test (no physical input)", WS_POPUP | WS_VISIBLE,
        left, 50, 400, 120, nullptr, nullptr, klass.hInstance, nullptr);
    HWND list = window ? CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_BORDER,
        8, 8, 380, 100, window, reinterpret_cast<HMENU>(6101), klass.hInstance, nullptr) : nullptr;
    if (!window || !list) { if (window) DestroyWindow(window); return 2; }
    originalList = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(list, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ListProcedure)));
    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"owned-test-item.txt"));
    RECT item = {};
    SendMessageW(list, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&item));
    POINT point = {item.left + 20, (item.top + item.bottom) / 2};
    ClientToScreen(list, &point);
    UpdateWindow(window);
    std::exception_ptr failure;
    bool tenMillisecondsTimedOut = false;
    std::thread worker([&] {
        try {
            DWORD_PTR result = 0;
            tenMillisecondsTimedOut = SendMessageTimeoutW(window, WM_NCHITTEST, 0,
                MAKELPARAM(point.x, point.y), SMTO_BLOCK | SMTO_ABORTIFHUNG, 10, &result) == 0;
            Check(tenMillisecondsTimedOut, "20 ms fixture must demonstrate failure of the former 10 ms hit deadline");
            Check(SendMessageTimeoutW(window, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y),
                SMTO_BLOCK | SMTO_ABORTIFHUNG, 50, &result) != 0 && result == HTCLIENT,
                "responsive delayed target must complete a 50 ms hit test");
            LocalDesktopInput input(0); // The only clicked window belongs to this test process.
            const auto mouse = [&](int status) {
                HWND hit = WindowFromPoint(point);
                DWORD process = 0;
                GetWindowThreadProcessId(hit, &process);
                Check(hit == list && process == GetCurrentProcessId() && IsWindow(list) && GetParent(list) == window,
                      "refusing proxy input outside this test's exact listbox HWND/PID");
                return input.Mouse(point.x, point.y, status);
            };
            Check(mouse(WM_LBUTTONDOWN), "first semantic down");
            Check(mouse(WM_LBUTTONUP), "first semantic up");
            Check(mouse(WM_LBUTTONDBLCLK), "second semantic down");
            Check(mouse(WM_LBUTTONUP), "second semantic up");
            Check(doubleClicks == 1 && selections >= 2, "two clicks must produce exactly one native double-click notification");
            Check(mouse(WM_LBUTTONDBLCLK), "pending double-click down");
            input.Release();
            Check(mouse(WM_LBUTTONUP), "up after cancelled gesture is ignored");
            Check(doubleClicks == 1, "cancel must not invoke the pending double-click");
        } catch (...) { failure = std::current_exception(); }
        PostMessageW(window, kFinished, 0, 0);
    });
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    worker.join();
    POINT finalCursor = {}; GetCursorPos(&finalCursor);
    const bool unchanged = originalCursor.x == finalCursor.x && originalCursor.y == finalCursor.y;
    const bool noCapture = GetCapture() == nullptr;
    DestroyWindow(window);
    UnregisterClassW(klass.lpszClassName, klass.hInstance);
    try {
        if (failure) std::rethrow_exception(failure);
        Check(unchanged, "proxy test must not warp the physical cursor");
        Check(noCapture && physicalMouseDowns == 0, "native listbox must not receive capture-stealing physical down messages");
        std::cout << "Local proxy delayed-hit regression passed: old10ms=timeout, current50ms=success, selections="
                  << selections << ", double_clicks=" << doubleClicks << ", hit_tests=" << hitTests
                  << ", physical_input=none.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
