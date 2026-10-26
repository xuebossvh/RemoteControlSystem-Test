#include "CaptureExclusion.h"
#include <cwchar>

namespace CaptureExclusion {
namespace {
constexpr DWORD kExcludeFromCapture = 0x00000011;
constexpr UINT kReportFailureMessage = WM_APP + 0x246;
struct State {
    HHOOK hook = NULL;
    bool attempted = false;
    bool supported = false;
    bool applying = false;
    bool pendingWarning = false;
    bool warningShown = false;
    DWORD error = ERROR_SUCCESS;
    const wchar_t* reason = L"";
};
thread_local State state;

void Failure(const wchar_t* reason, DWORD error) {
    if (state.warningShown || state.pendingWarning) return;
    state.reason = reason;
    state.error = error;
    state.pendingWarning = true;
    // Do not open a modal dialog recursively inside a window-creation hook.
    PostThreadMessageW(GetCurrentThreadId(), kReportFailureMessage, 0, 0);
}

bool IsSupported() {
    // RtlGetVersion returns the actual build even without a supportedOS manifest.
    typedef LONG (WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
    const auto versionFunction = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!versionFunction || versionFunction(&version) != 0 ||
        version.dwMajorVersion < 10 ||
        (version.dwMajorVersion == 10 && version.dwBuildNumber < 19041)) {
        Failure(L"需要 Windows 10 版本 2004（内部版本 19041）或更新系统。", ERROR_OLD_WIN_VERSION);
        return false;
    }
    HMODULE compositor = LoadLibraryW(L"dwmapi.dll");
    typedef HRESULT (WINAPI* IsCompositionEnabledFn)(BOOL*);
    const auto checkComposition = compositor ? reinterpret_cast<IsCompositionEnabledFn>(
        GetProcAddress(compositor, "DwmIsCompositionEnabled")) : NULL;
    BOOL enabled = FALSE;
    const HRESULT result = checkComposition ? checkComposition(&enabled) : E_FAIL;
    if (compositor) FreeLibrary(compositor);
    if (FAILED(result) || !enabled) {
        Failure(L"桌面窗口管理器的桌面合成功能不可用。", ERROR_NOT_SUPPORTED);
        return false;
    }
    return true;
}

void Apply(HWND window, bool reportFailure) {
    if (!state.supported || state.applying || !IsWindow(window)) return;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || thread != GetCurrentThreadId() ||
        (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) != 0 ||
        GetParent(window) == HWND_MESSAGE || GetAncestor(window, GA_ROOT) != window) return;

    state.applying = true;
    DWORD affinity = 0;
    bool applied = GetWindowDisplayAffinity(window, &affinity) && affinity == kExcludeFromCapture;
    if (!applied) {
        SetLastError(ERROR_SUCCESS);
        applied = SetWindowDisplayAffinity(window, kExcludeFromCapture) != FALSE;
        if (applied) applied = GetWindowDisplayAffinity(window, &affinity) && affinity == kExcludeFromCapture;
        if (!applied && reportFailure) {
            DWORD error = GetLastError();
            Failure(L"Windows 未能将某个客户端顶层窗口排除在屏幕采集之外。",
                error ? error : ERROR_NOT_SUPPORTED);
        }
    }
    state.applying = false;
}

BOOL CALLBACK ApplyExistingWindow(HWND window, LPARAM) {
    Apply(window, IsWindowVisible(window) != FALSE);
    return TRUE;
}

LRESULT CALLBACK BeforeWindowMessage(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && lParam) {
        const CWPSTRUCT& message = *reinterpret_cast<const CWPSTRUCT*>(lParam);
        switch (message.message) {
        case WM_CREATE:
        case WM_INITDIALOG:
            // Best effort while still hidden; retry before it becomes visible.
            Apply(message.hwnd, false);
            break;
        case WM_SHOWWINDOW:
            if (message.wParam) Apply(message.hwnd, true);
            break;
        case WM_WINDOWPOSCHANGING:
            if (message.lParam &&
                (reinterpret_cast<const WINDOWPOS*>(message.lParam)->flags & SWP_SHOWWINDOW))
                Apply(message.hwnd, true);
            break;
        default:
            break;
        }
    }
    return CallNextHookEx(state.hook, code, wParam, lParam);
}
}

bool InitializeForCurrentThread() {
    if (state.attempted) return state.hook != NULL;
    state.attempted = true;
    state.supported = IsSupported();
    if (!state.supported) return false;
    state.hook = SetWindowsHookExW(WH_CALLWNDPROC, BeforeWindowMessage, NULL, GetCurrentThreadId());
    if (!state.hook) {
        Failure(L"无法安装客户端窗口创建监听。", GetLastError());
        return false;
    }
    EnumThreadWindows(GetCurrentThreadId(), ApplyExistingWindow, 0);
    return true;
}

void ShutdownForCurrentThread() {
    if (state.hook) { UnhookWindowsHookEx(state.hook); state.hook = NULL; }
}

void ShowPendingFailure(HWND owner) {
    if (!state.pendingWarning || state.warningShown) return;
    state.pendingWarning = false;
    state.warningShown = true; // Also prevents recursion while creating this notice.
    wchar_t text[768] = {};
    swprintf_s(text,
        L"无法启用客户端窗口的屏幕采集排除。\n\n%s\nWindows 错误码：%lu\n\n"
        L"本机连接时，远程画面仍可能包含客户端窗口并产生重复画面。请勿将当前状态视为排除成功。",
        state.reason, state.error);
    MessageBoxW(IsWindow(owner) ? owner : NULL, text, L"屏幕采集排除未生效", MB_OK | MB_ICONWARNING);
}

bool ProcessThreadMessage(const MSG& message) {
    if (message.hwnd || message.message != kReportFailureMessage) return false;
    ShowPendingFailure();
    return true;
}
}
