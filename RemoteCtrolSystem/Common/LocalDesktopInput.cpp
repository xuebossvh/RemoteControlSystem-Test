#include "LocalDesktopInput.h"
#include "RemoteInput.h"
#include <oleacc.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <array>
#include <cwchar>
#include <limits>
#include <algorithm>
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ole32.lib")
using Microsoft::WRL::ComPtr;

namespace {
constexpr int kKeyDown = 0xFFFE;
constexpr int kKeyUp = 0xFFFF;
bool Send(HWND target, UINT message, WPARAM wp = 0, LPARAM lp = 0, DWORD_PTR* reply = nullptr, UINT timeout = 50) {
    if (!IsWindow(target)) return false;
    DWORD_PTR value = 0;
    const bool ok = SendMessageTimeoutW(target, message, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, timeout, &value) != 0;
    if (reply) *reply = value;
    return ok;
}
LPARAM Coordinates(POINT point) { return MAKELPARAM(static_cast<SHORT>(point.x), static_cast<SHORT>(point.y)); }
bool ClassIs(HWND window, const wchar_t* name) {
    wchar_t actual[128] = {};
    return GetClassNameW(window, actual, 128) && _wcsicmp(actual, name) == 0;
}
POINT ClientPoint(HWND target, POINT screen) { ScreenToClient(target, &screen); return screen; }
}

struct LocalDesktopInput::Impl {
    enum class Kind { None, Button, ListBox, Edit, Accessible };
    explicit Impl(DWORD excluded) : excludedProcess(excluded) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninitializeCOM = SUCCEEDED(initialized);
        HRESULT created = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(automation.GetAddressOf()));
        if (FAILED(created)) CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(automation.GetAddressOf()));
        ComPtr<IUIAutomation2> newer;
        if (automation && SUCCEEDED(automation.As(&newer))) {
            newer->put_ConnectionTimeout(100);
            newer->put_TransactionTimeout(100);
        }
        dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) attribute = reinterpret_cast<DwmAttribute>(GetProcAddress(dwm, "DwmGetWindowAttribute"));
    }
    ~Impl() {
        Cancel(); accessible.Reset(); element.Reset(); automation.Reset();
        if (dwm) FreeLibrary(dwm);
        if (uninitializeCOM) CoUninitialize();
    }
    typedef HRESULT (WINAPI* DwmAttribute)(HWND, DWORD, PVOID, DWORD);
    DWORD excludedProcess;
    HMODULE dwm = nullptr;
    DwmAttribute attribute = nullptr;
    bool uninitializeCOM = false;
    ComPtr<IUIAutomation> automation;
    HWND hover = nullptr, pressed = nullptr, keyboard = nullptr;
    Kind kind = Kind::None;
    POINT last = {};
    bool leftPressed = false, invokeOnUp = false, buttonArmed = false;
    int selectionAnchor = -1;
    unsigned long long cancelEpoch = 0;
    std::array<BYTE, 256> keyState{};
    std::array<HWND, 256> keyTargets{};
    ComPtr<IAccessible> accessible;
    LONG accessibleChild = CHILDID_SELF;
    ComPtr<IUIAutomationElement> element;

    bool Allowed(HWND window) const {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (!IsWindow(window) || process == excludedProcess || !IsWindowVisible(window) || !IsWindowEnabled(window)) return false;
        return true;
    }
    bool TopAllowed(HWND window) const {
        if (!Allowed(window) || IsIconic(window)) return false;
        DWORD cloaked = 0;
        return !attribute || FAILED(attribute(window, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) || !cloaked;
    }
    HWND Hit(POINT screen, bool precise) {
        struct Search { Impl* self; POINT point; HWND found; bool precise; } search = {this, screen, nullptr, precise};
        EnumWindows([](HWND window, LPARAM parameter)->BOOL {
            Search& value = *reinterpret_cast<Search*>(parameter);
            RECT rectangle = {};
            if (!value.self->TopAllowed(window) || !GetWindowRect(window, &rectangle) || !PtInRect(&rectangle, value.point)) return TRUE;
            // Hover never waits for another process's message pump. Geometry
            // suffices to avoid title bars; exact custom hit testing is reserved
            // for button actions and has a short bounded wait.
            RECT client = {};
            if (!GetClientRect(window, &client)) return FALSE;
            MapWindowPoints(window, NULL, reinterpret_cast<POINT*>(&client), 2);
            if (!PtInRect(&client, value.point)) return FALSE;
            if (value.precise) {
                DWORD_PTR hit = HTNOWHERE;
                if (!Send(window, WM_NCHITTEST, 0, Coordinates(value.point), &hit, 50)) return FALSE;
                if (static_cast<LRESULT>(hit) == HTTRANSPARENT || static_cast<LRESULT>(hit) == HTNOWHERE) return TRUE;
                if (hit != HTCLIENT) return FALSE;
            }
            value.found = window;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&search));
        HWND target = search.found;
        for (int depth = 0; target && depth < 32; ++depth) {
            const POINT point = ClientPoint(target, screen);
            HWND child = ChildWindowFromPointEx(target, point, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
            if (!child || child == target || !Allowed(child)) break;
            target = child;
        }
        return target;
    }
    WPARAM Modifiers() const {
        return (leftPressed ? MK_LBUTTON : 0) |
            ((keyState[VK_CONTROL] & 0x80) ? MK_CONTROL : 0) |
            ((keyState[VK_SHIFT] & 0x80) ? MK_SHIFT : 0);
    }
    void Hover(HWND target, POINT screen) {
        if (hover != target) {
            if (Allowed(hover)) PostMessageW(hover, WM_MOUSELEAVE, 0, 0);
            hover = target;
        }
        if (Allowed(hover)) PostMessageW(hover, WM_MOUSEMOVE, Modifiers(), Coordinates(ClientPoint(hover, screen)));
    }
    bool ShellOrList(HWND target) const {
        if (ClassIs(target, L"SysListView32") || ClassIs(target, L"SysTreeView32")) return true;
        for (HWND window = target; window; window = GetParent(window)) {
            if (ClassIs(window, L"CabinetWClass") || ClassIs(window, L"ExploreWClass") ||
                ClassIs(window, L"Progman") || ClassIs(window, L"WorkerW") ||
                ClassIs(window, L"DirectUIHWND") || ClassIs(window, L"UIItemsView")) return true;
        }
        return false;
    }
    bool PickAccessible(HWND target, POINT screen, bool doubleClick) {
        accessible.Reset(); element.Reset();
        ComPtr<IAccessible> current;
        if (SUCCEEDED(AccessibleObjectFromWindow(target, OBJID_CLIENT, __uuidof(IAccessible),
                                                reinterpret_cast<void**>(current.GetAddressOf())))) {
            for (int depth = 0; current && depth < 16; ++depth) {
                VARIANT hit; VariantInit(&hit);
                if (FAILED(current->accHitTest(screen.x, screen.y, &hit))) { VariantClear(&hit); break; }
                if (hit.vt == VT_DISPATCH && hit.pdispVal) {
                    ComPtr<IAccessible> next;
                    hit.pdispVal->QueryInterface(IID_PPV_ARGS(next.GetAddressOf()));
                    VariantClear(&hit);
                    if (!next || next.Get() == current.Get()) break;
                    current = next; continue;
                }
                if (hit.vt == VT_I4) {
                    VARIANT role; VariantInit(&role);
                    const HRESULT roleResult = current->get_accRole(hit, &role);
                    const bool item = SUCCEEDED(roleResult) && role.vt == VT_I4 &&
                        (role.lVal == ROLE_SYSTEM_LISTITEM || role.lVal == ROLE_SYSTEM_OUTLINEITEM);
                    VariantClear(&role);
                    if (item && current->accSelect(SELFLAG_TAKESELECTION, hit) == S_OK) {
                        accessible = current; accessibleChild = hit.lVal; invokeOnUp = doubleClick;
                        VariantClear(&hit); return true;
                    }
                }
                VariantClear(&hit); break;
            }
        }
        // Modern Explorer can expose its file items only through UI Automation.
        // Start at the already selected underlying HWND, never ElementFromPoint.
        if (!automation) return false;
        ComPtr<IUIAutomationElement> root;
        if (FAILED(automation->ElementFromHandle(target, root.GetAddressOf()))) return false;
        ComPtr<IUIAutomationCondition> condition;
        automation->CreateTrueCondition(condition.GetAddressOf());
        if (!condition) return false;
        ComPtr<IUIAutomationElementArray> items;
        if (FAILED(root->FindAll(TreeScope_Subtree, condition.Get(), items.GetAddressOf()))) return false;
        int length = 0; items->get_Length(&length);
        LONGLONG bestArea = (std::numeric_limits<LONGLONG>::max)();
        for (int index = 0; index < length && index < 2048; ++index) {
            ComPtr<IUIAutomationElement> candidate;
            RECT rectangle = {}; CONTROLTYPEID type = 0; BOOL enabled = FALSE;
            if (FAILED(items->GetElement(index, candidate.GetAddressOf())) ||
                FAILED(candidate->get_CurrentBoundingRectangle(&rectangle)) || !PtInRect(&rectangle, screen) ||
                FAILED(candidate->get_CurrentIsEnabled(&enabled)) || !enabled ||
                FAILED(candidate->get_CurrentControlType(&type)) ||
                (type != UIA_ListItemControlTypeId && type != UIA_TreeItemControlTypeId && type != UIA_DataItemControlTypeId)) continue;
            const LONGLONG area = static_cast<LONGLONG>(rectangle.right-rectangle.left) * (rectangle.bottom-rectangle.top);
            if (area >= 0 && area < bestArea) { element = candidate; bestArea = area; }
        }
        if (!element) return false;
        ComPtr<IUIAutomationSelectionItemPattern> selection;
        if (FAILED(element->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(selection.GetAddressOf()))) ||
            FAILED(selection->Select())) { element.Reset(); return false; }
        invokeOnUp = doubleClick;
        return true;
    }
    bool InvokeAccessible() {
        // Invoking a file may activate another app and re-enter Client::Release.
        // Keep these interfaces alive independently of the mutable gesture state.
        const ComPtr<IAccessible> actionAccessible = accessible;
        const ComPtr<IUIAutomationElement> actionElement = element;
        const LONG actionChild = accessibleChild;
        if (actionAccessible) {
            VARIANT child; VariantInit(&child); child.vt = VT_I4; child.lVal = actionChild;
            return actionAccessible->accDoDefaultAction(child) == S_OK;
        }
        if (!actionElement) return false;
        ComPtr<IUIAutomationInvokePattern> invoke;
        if (SUCCEEDED(actionElement->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(invoke.GetAddressOf()))))
            return SUCCEEDED(invoke->Invoke());
        ComPtr<IUIAutomationLegacyIAccessiblePattern> legacy;
        return SUCCEEDED(actionElement->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(legacy.GetAddressOf()))) &&
            SUCCEEDED(legacy->DoDefaultAction());
    }
    bool SelectListBox(HWND target, POINT screen, bool dragging) {
        DWORD_PTR hit = 0;
        if (!Send(target, LB_ITEMFROMPOINT, 0, Coordinates(ClientPoint(target, screen)), &hit) || HIWORD(hit)) return false;
        const int index = LOWORD(hit);
        const LONG_PTR style = GetWindowLongPtrW(target, GWL_STYLE);
        bool selected = false;
        if (style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) {
            if (!dragging) { Send(target, LB_SETSEL, FALSE, -1); selectionAnchor = index; }
            selected = Send(target, LB_SELITEMRANGEEX, selectionAnchor, index);
        } else selected = Send(target, LB_SETCURSEL, index);
        if (selected && (style & LBS_NOTIFY))
            Send(GetParent(target), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(target), LBN_SELCHANGE), reinterpret_cast<LPARAM>(target));
        return selected;
    }
    bool EditSelection(HWND target, POINT screen, bool dragging) {
        DWORD_PTR hit = 0;
        if (!Send(target, EM_CHARFROMPOS, 0, Coordinates(ClientPoint(target, screen)), &hit) || hit == static_cast<DWORD_PTR>(-1)) return false;
        const int index = LOWORD(hit);
        if (!dragging) selectionAnchor = index;
        return Send(target, EM_SETSEL, selectionAnchor, index);
    }
    bool Down(HWND target, POINT screen, bool doubleClick) {
        // A new target must never inherit keyboard input from an earlier Edit.
        // Cancel old virtual keys safely before taking the gesture generation.
        Cancel();
        const unsigned long long generation = cancelEpoch;
        if (!Allowed(target)) return false;
        bool ok = false;
        kind = Kind::None; invokeOnUp = false;
        if (ClassIs(target, L"Button")) {
            kind = Kind::Button; buttonArmed = true;
            ok = Send(target, BM_SETSTATE, TRUE);
        } else if (ClassIs(target, L"ListBox")) {
            kind = Kind::ListBox; invokeOnUp = doubleClick;
            ok = SelectListBox(target, screen, false);
        } else if (ClassIs(target, L"Edit")) {
            kind = Kind::Edit;
            ok = EditSelection(target, screen, false);
        } else if (ShellOrList(target)) {
            kind = Kind::Accessible;
            ok = PickAccessible(target, screen, doubleClick);
        }
        if (!ok || generation != cancelEpoch) {
            if (kind == Kind::Button) Send(target, BM_SETSTATE, FALSE);
            kind = Kind::None; pressed = nullptr; keyboard = nullptr; leftPressed = false;
            invokeOnUp = false; accessible.Reset(); element.Reset();
            return false;
        }
        pressed = target; keyboard = target; leftPressed = true;
        return true;
    }
    bool Drag(POINT screen) {
        if (!Allowed(pressed)) return false;
        switch (kind) {
        case Kind::Button: {
            RECT bounds = {}; GetClientRect(pressed, &bounds);
            buttonArmed = PtInRect(&bounds, ClientPoint(pressed, screen)) != FALSE;
            return Send(pressed, BM_SETSTATE, buttonArmed);
        }
        case Kind::ListBox: return SelectListBox(pressed, screen, true);
        case Kind::Edit: return EditSelection(pressed, screen, true);
        case Kind::Accessible: return false; // File drag/drop needs an actual OLE drag session.
        default: return false;
        }
    }
    bool Up(POINT screen) {
        if (!leftPressed) return true;
        HWND target = pressed;
        const Kind action = kind;
        const bool invoke = invokeOnUp;
        // Clear state before a control notification can re-enter the Client.
        leftPressed = false; pressed = nullptr; kind = Kind::None; invokeOnUp = false;
        if (!Allowed(target)) return false;
        bool result = true;
        if (action == Kind::Button) {
            RECT bounds = {}; GetClientRect(target, &bounds);
            Send(target, BM_SETSTATE, FALSE);
            if (buttonArmed && PtInRect(&bounds, ClientPoint(target, screen))) result = Send(target, BM_CLICK);
        } else if (action == Kind::ListBox && invoke) {
            result = Send(GetParent(target), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(target), LBN_DBLCLK), reinterpret_cast<LPARAM>(target));
        } else if (action == Kind::Accessible && invoke) result = InvokeAccessible();
        accessible.Reset(); element.Reset();
        return result;
    }
    LPARAM KeyParameters(int key, bool released, bool repeat, HWND target) {
        const DWORD thread = GetWindowThreadProcessId(target, nullptr);
        const UINT scan = MapVirtualKeyExW(key, MAPVK_VK_TO_VSC_EX, GetKeyboardLayout(thread));
        return 1 | ((scan & 0xff) << 16) | ((scan & 0xff00) ? (1L << 24) : 0) |
            ((repeat || released) ? (1L << 30) : 0) | (released ? static_cast<LONG>(0x80000000UL) : 0);
    }
    bool Key(int key, int status) {
        if (key < 0 || key > 255 || (status != kKeyDown && status != kKeyUp)) return false;
        const bool released = status == kKeyUp, repeat = keyTargets[key] != nullptr;
        if (released && !repeat) return true;
        HWND target = repeat ? keyTargets[key] : keyboard;
        if (!Allowed(target)) { keyState[key] = 0; keyTargets[key] = nullptr; return false; }
        keyState[key] = released ? 0 : 0x80;
        keyTargets[key] = released ? nullptr : target;
        const LPARAM parameters = KeyParameters(key, released, repeat, target);
        if (!Send(target, released ? WM_KEYUP : WM_KEYDOWN, key, parameters)) return false;
        if (!released && !(keyState[VK_CONTROL] & 0x80) && !(keyState[VK_MENU] & 0x80)) {
            auto translated = keyState;
            translated[VK_CAPITAL] = static_cast<BYTE>(GetKeyState(VK_CAPITAL) & 1);
            wchar_t characters[8] = {};
            const DWORD thread = GetWindowThreadProcessId(target, nullptr);
            const UINT scan = MapVirtualKeyExW(key, MAPVK_VK_TO_VSC, GetKeyboardLayout(thread));
            const int count = ToUnicodeEx(key, scan, translated.data(), characters, 8, 4, GetKeyboardLayout(thread));
            // Synchronous KEYDOWN does not pass through TranslateMessage. Send
            // basic text ourselves; do not alter the thread's dead-key state.
            for (int index = 0; index < count && index < 8; ++index) Send(target, WM_CHAR, characters[index], parameters);
        }
        return true;
    }
    void Cancel() {
        ++cancelEpoch;
        if (kind == Kind::Button && Allowed(pressed)) Send(pressed, BM_SETSTATE, FALSE);
        leftPressed = false; pressed = nullptr; kind = Kind::None; invokeOnUp = false;
        accessible.Reset(); element.Reset();
        for (int key = 0; key < 256; ++key) {
            HWND target = keyTargets[key];
            keyTargets[key] = nullptr; keyState[key] = 0;
            if (Allowed(target)) {
                // A virtual Space key-up on a Button would execute a click.
                // Cancellation clears its pressed visual/state instead.
                if (ClassIs(target, L"Button") && key == VK_SPACE) {
                    Send(target, WM_CANCELMODE);
                    Send(target, BM_SETSTATE, FALSE);
                } else Send(target, WM_KEYUP, key, KeyParameters(key, true, true, target));
            }
        }
        if (Allowed(hover)) PostMessageW(hover, WM_MOUSELEAVE, 0, 0);
        hover = nullptr; keyboard = nullptr;
    }
    bool Mouse(int x, int y, int status) {
        POINT screen = {x, y}; last = screen;
        int delta = 0; bool horizontal = false;
        const bool buttonAction = status != 0 && status != WM_MOUSEMOVE && !RemoteInput::DecodeWheel(status, delta, &horizontal);
        HWND target = leftPressed ? pressed : Hit(screen, buttonAction);
        if (RemoteInput::DecodeWheel(status, delta, &horizontal)) {
            return Allowed(target) && Send(target, horizontal ? 0x020E : WM_MOUSEWHEEL,
                MAKEWPARAM(Modifiers(), static_cast<SHORT>(delta)), Coordinates(screen));
        }
        if (status == 0 || status == WM_MOUSEMOVE) {
            Hover(target, screen);
            return leftPressed ? Drag(screen) : target != nullptr;
        }
        if (status == WM_LBUTTONDOWN || status == WM_LBUTTONDBLCLK) {
            if (leftPressed) return true;
            return Down(target, screen, status == WM_LBUTTONDBLCLK);
        }
        if (status == WM_LBUTTONUP) return Up(screen);
        if (status == WM_RBUTTONUP || status == WM_MBUTTONUP) return true;
        // Background context menus, middle actions, raw-input controls and
        // non-client drags are not emulated by unsafe physical input fallback.
        Cancel();
        return false;
    }
};
LocalDesktopInput::LocalDesktopInput(DWORD excludedProcess) : impl_(new Impl(excludedProcess)) {}
LocalDesktopInput::~LocalDesktopInput() {}
bool LocalDesktopInput::Mouse(int x, int y, int status) { return impl_->Mouse(x, y, status); }
bool LocalDesktopInput::Keyboard(int key, int status) { return impl_->Key(key, status); }
void LocalDesktopInput::Release() { impl_->Cancel(); }

HWND LocalDesktopInput::TargetWindow() const { return impl_->pressed ? impl_->pressed : impl_->keyboard; }
