#include "MouseKeybdEvent.h"
#include "../Common/RemoteInput.h"

MouseKeybdEvent::MouseKeybdEvent() {}
MouseKeybdEvent::~MouseKeybdEvent() {}
namespace {
bool SendAt(int x, int y, const DWORD* actions, UINT count) {
    if (!actions || count == 0 || count > 4) return false;
    INPUT events[4] = {};
    for (UINT index = 0; index < count; ++index)
        if (!RemoteInput::MakePrimaryMouseInput(x, y, actions[index], events[index])) return false;
    return SendInput(count, events, sizeof(INPUT)) == count;
}
void SendKey(int key, bool released, bool extended) {
    if (key < 0 || key > 255) return;
    INPUT event = RemoteInput::MakeKeyboardInput(key, released, extended);
    SendInput(1, &event, sizeof(INPUT));
}
}

bool MouseKeybdEvent::Move(int x, int y) {
    const DWORD actions[] = {0};
    return SendAt(x, y, actions, 1);
}
void MouseKeybdEvent::LeftClick(int x, int y) {
    const DWORD actions[] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP};
    SendAt(x, y, actions, 2);
}
void MouseKeybdEvent::RightClick(int x, int y) {
    const DWORD actions[] = {MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP};
    SendAt(x, y, actions, 2);
}
void MouseKeybdEvent::LeftClickDown(int x, int y) {
    const DWORD actions[] = {MOUSEEVENTF_LEFTDOWN};
    SendAt(x, y, actions, 1);
}
void MouseKeybdEvent::LeftClickUp(int x, int y) {
    const DWORD actions[] = {MOUSEEVENTF_LEFTUP};
    SendAt(x, y, actions, 1);
}
void MouseKeybdEvent::RightClickDown(int x, int y) {
    const DWORD actions[] = {MOUSEEVENTF_RIGHTDOWN};
    SendAt(x, y, actions, 1);
}
void MouseKeybdEvent::RightClickUp(int x, int y) {
    const DWORD actions[] = {MOUSEEVENTF_RIGHTUP};
    SendAt(x, y, actions, 1);
}
void MouseKeybdEvent::LeftDbClick(int x, int y) {
    const DWORD actions[] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP};
    SendAt(x, y, actions, 4);
}
void MouseKeybdEvent::RightDbClick(int x, int y) {
    const DWORD actions[] = {MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP};
    SendAt(x, y, actions, 4);
}
void MouseKeybdEvent::MouseWheel(int delta, bool horizontal) {
    INPUT event = {};
    event.type = INPUT_MOUSE;
    event.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    event.mi.mouseData = static_cast<DWORD>(delta);
    event.mi.dwExtraInfo = RemoteInput::kExtraInfoMarker;
    SendInput(1, &event, sizeof(INPUT));
}
void MouseKeybdEvent::GenerateKey(int key, BOOL extended) {
    if (key < 0 || key > 255) return;
    INPUT events[] = {RemoteInput::MakeKeyboardInput(key, false, extended != FALSE),
                      RemoteInput::MakeKeyboardInput(key, true, extended != FALSE)};
    SendInput(2, events, sizeof(INPUT));
}
void MouseKeybdEvent::KeyDown(int key, BOOL extended) { SendKey(key, false, extended != FALSE); }
void MouseKeybdEvent::KeyUp(int key, BOOL extended) { SendKey(key, true, extended != FALSE); }

void MouseKeybdEvent::MiddleClickDown(int x, int y) {
    const DWORD actions[] = {MOUSEEVENTF_MIDDLEDOWN};
    SendAt(x, y, actions, 1);
}
void MouseKeybdEvent::MiddleClickUp(int x, int y) {
    const DWORD actions[] = {MOUSEEVENTF_MIDDLEUP};
    SendAt(x, y, actions, 1);
}
void MouseKeybdEvent::ReleaseButtons(DWORD buttons) {
    DWORD flags = 0;
    if (buttons & MK_LBUTTON) flags |= MOUSEEVENTF_LEFTUP;
    if (buttons & MK_RBUTTON) flags |= MOUSEEVENTF_RIGHTUP;
    if (buttons & MK_MBUTTON) flags |= MOUSEEVENTF_MIDDLEUP;
    if (!flags) return;
    INPUT event = {};
    event.type = INPUT_MOUSE;
    event.mi.dwFlags = flags;
    event.mi.dwExtraInfo = RemoteInput::kExtraInfoMarker;
    SendInput(1, &event, sizeof(INPUT));
}
