#pragma once
#include <windows.h>
#include <shellapi.h>
#include <string>

// Desktop GDI capture, window layout and injected coordinates must use the same
// physical pixel space. Set before creating any application windows; this only
// changes this process, never the user's display scaling setting.
inline bool InitializeApplicationDpi()
{
    typedef BOOL (WINAPI* DpiFunction)();
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto isAware = reinterpret_cast<DpiFunction>(GetProcAddress(user32, "IsProcessDPIAware"));
    const auto setAware = reinterpret_cast<DpiFunction>(GetProcAddress(user32, "SetProcessDPIAware"));
    return (isAware && isAware()) || (setAware && setAware());
}

// Keep writable configuration separate from the legacy source samples.
inline bool InitializeRuntimeDirectory(const wchar_t* leaf)
{
    wchar_t executable[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    std::wstring path(executable);
    path.erase(path.find_last_of(L"\\/") + 1);
    path += leaf;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments) {
        for (int i = 1; i + 1 < count; ++i) {
            if (std::wstring(arguments[i]) == L"--data-dir") path = arguments[++i];
        }
        LocalFree(arguments);
    }
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    return SetCurrentDirectoryW(path.c_str()) != FALSE;
}
