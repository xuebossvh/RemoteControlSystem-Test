#pragma once
#include <windows.h>
#include <cstdint>

// Shared by the authenticated Server injector and the Client's message filter.
// This identifies our own synthetic input; it is not an authorization token.
namespace RemoteInput {
inline int EncodeWheel(int delta, bool horizontal = false) {
    return static_cast<int>((static_cast<unsigned>(static_cast<WORD>(delta)) << 16) | (horizontal ? 0x020E : WM_MOUSEWHEEL));
}
inline bool DecodeWheel(int status, int& delta, bool* horizontal = nullptr) {
    const unsigned packed = static_cast<unsigned>(status);
    const unsigned kind = packed & 0xffffu;
    if (kind != WM_MOUSEWHEEL && kind != 0x020E) return false;
    if (horizontal) *horizontal = kind == 0x020E;
    delta = static_cast<SHORT>(packed >> 16);
    return delta != 0;
}
constexpr ULONG_PTR kExtraInfoMarker = static_cast<ULONG_PTR>(0x52435331UL);
inline bool IsInjectedExtraInfo(ULONG_PTR value) { return value == kExtraInfoMarker; }
inline bool IsCurrentMessageInjected() {
    return IsInjectedExtraInfo(static_cast<ULONG_PTR>(GetMessageExtraInfo()));
}

// Windows absolute mouse coordinates address the primary screen in [0,65535].
// Use 64-bit arithmetic and preserve both physical-pixel endpoints.
inline LONG NormalizePrimaryCoordinate(int pixel, int extent) {
    if (pixel <= 0 || extent <= 1) return 0;
    if (pixel >= extent - 1) return 65535;
    return static_cast<LONG>((static_cast<std::int64_t>(pixel) * 65535 + (extent - 1) / 2) / (extent - 1));
}
inline bool MakePrimaryMouseInput(int x, int y, DWORD buttonFlags, INPUT& input) {
    const int width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);
    if (x < 0 || y < 0 || x >= width || y >= height) return false;
    input = INPUT{};
    input.type = INPUT_MOUSE;
    input.mi.dx = NormalizePrimaryCoordinate(x, width);
    input.mi.dy = NormalizePrimaryCoordinate(y, height);
    // Do not merge marked moves with a following physical mouse movement.
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | 0x2000 /* MOVE_NOCOALESCE */ | buttonFlags;
    input.mi.dwExtraInfo = kExtraInfoMarker;
    return true;
}
inline INPUT MakeKeyboardInput(int key, bool released, bool extended) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(key);
    input.ki.dwFlags = (released ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    input.ki.dwExtraInfo = kExtraInfoMarker;
    return input;
}
}
