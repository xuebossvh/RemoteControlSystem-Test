#pragma once
#include <windows.h>

// Only the calling Client UI thread and its own top-level windows are affected.
// Install before creating dialogs; owned popups are top-level windows too.
namespace CaptureExclusion {
bool InitializeForCurrentThread();
void ShutdownForCurrentThread();
// Route thread messages before normal application input handling.
bool ProcessThreadMessage(const MSG& message);
// Fallback for a pending diagnostic before the first message loop/on idle.
void ShowPendingFailure(HWND owner = NULL);
}
