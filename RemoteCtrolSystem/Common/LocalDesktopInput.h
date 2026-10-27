#pragma once
#include <windows.h>
#include <memory>

// Opt-in same-input-desktop proxy, enabled only after peer identity validation.
// Never injects physical input or warps the shared desktop cursor. Native
// control/accessibility semantics are used instead of capture-stealing DOWNs.
// Unsupported actions return false; callers must not fall back to SendInput.
class LocalDesktopInput {
public:
    explicit LocalDesktopInput(DWORD excludedProcess = GetCurrentProcessId());
    ~LocalDesktopInput();
    bool Mouse(int screenX, int screenY, int encodedStatus);
    bool Keyboard(int key, int status);
    // Cancel pending actions and release virtual state; never click or invoke.
    void Release();
    HWND TargetWindow() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    LocalDesktopInput(const LocalDesktopInput&) = delete;
    LocalDesktopInput& operator=(const LocalDesktopInput&) = delete;
};
