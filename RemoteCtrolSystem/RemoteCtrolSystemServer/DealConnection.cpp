#include "StdAfx.h"
#include "DealConnection.h"
#include "IpRuleFile.h"
#include "SendImageThread.h"
#include "MouseKeybdEvent.h"
#include "TabConnectionDlg.h"
#include "UserFile.h"
#include "WndClassList.h"
#include "../Message/SetupMsg.h"
#include "../Message/UserMsg.h"
#include "../Message/KeyboardMsg.h"
#include "../Message/MouseMsg.h"
#include <atlconv.h>
#include <cstring>
#include <set>
#include "../Common/RemoteInput.h"

DealConnection::~DealConnection() {}
namespace {
struct InjectedInputState {
    DWORD buttons = 0;
    std::set<int> keys;
    void Release() {
        MouseKeybdEvent::ReleaseButtons(buttons);
        buttons = 0;
        for (int key : keys) MouseKeybdEvent::KeyUp(key, FALSE);
        keys.clear();
    }
};
void HandleMouse(MouseMsg& message, InjectedInputState& state) {
    const int x = message.GetMouseX(), y = message.GetMouseY();
    if (x < 0 || y < 0 || x >= GetSystemMetrics(SM_CXSCREEN) || y >= GetSystemMetrics(SM_CYSCREEN)) return;
    int wheel = 0;
    bool horizontal = false;
    if (RemoteInput::DecodeWheel(message.GetMouseStatus(), wheel, &horizontal)) {
        MouseKeybdEvent::Move(x, y);
        MouseKeybdEvent::MouseWheel(wheel, horizontal);
        return;
    }
    switch (message.GetMouseStatus()) {
    case 0: MouseKeybdEvent::Move(x, y); break;
    case RCS_MOUSE_LEFT_DOWN:
    case RCS_MOUSE_LEFT_DBLCLK: // Windows sends DOWN, UP, DBLCLK, UP for two physical clicks.
        state.buttons |= MK_LBUTTON; MouseKeybdEvent::LeftClickDown(x, y); break;
    case RCS_MOUSE_LEFT_UP:
        if (state.buttons & MK_LBUTTON) MouseKeybdEvent::LeftClickUp(x, y);
        state.buttons &= ~MK_LBUTTON; break;
    case RCS_MOUSE_RIGHT_DOWN:
    case RCS_MOUSE_RIGHT_DBLCLK:
        state.buttons |= MK_RBUTTON; MouseKeybdEvent::RightClickDown(x, y); break;
    case RCS_MOUSE_RIGHT_UP:
        if (state.buttons & MK_RBUTTON) MouseKeybdEvent::RightClickUp(x, y);
        state.buttons &= ~MK_RBUTTON; break;
    case RCS_MOUSE_MINDLE_DOWN:
    case RCS_MOUSE_MINDLE_DBLCLK:
        state.buttons |= MK_MBUTTON; MouseKeybdEvent::MiddleClickDown(x, y); break;
    case RCS_MOUSE_MINDLE_UP:
        if (state.buttons & MK_MBUTTON) MouseKeybdEvent::MiddleClickUp(x, y);
        state.buttons &= ~MK_MBUTTON; break;
    default: break;
    }
}
void HandleKeyboard(KeyboardMsg& message, InjectedInputState& state) {
    const int key = message.GetKey(), status = message.GetKeyStatus();
    if (key == RCS_KEY_TASKMGR) {
        if (status == RCS_KEY_DOWN) WinExec("taskmgr.exe", SW_SHOW);
        return;
    }
    if (key < 0 || key > 255) return;
    if (status == RCS_KEY_DOWN) {
        state.keys.insert(key);
        MouseKeybdEvent::KeyDown(key, FALSE);
    } else if (status == RCS_KEY_UP && state.keys.erase(key)) {
        MouseKeybdEvent::KeyUp(key, FALSE);
    }
}

}
void DealConnection::Run() {
    USES_CONVERSION;
    bool listed = false;
    InjectedInputState inputState;
    SendImageThread images;
    images.SetConnection(&connection);
    try {
        const std::string address = connection.getConnectedAddress().GetIPString(0);
        CIpRuleFile rules;
        SetupMsg permission;
        const bool allowed = rules.GetPowerByIp(CString(address.c_str())) != 0;
        permission.PacketMsg(RCS_SETUP_MSG_PERMISSION, RCS_PERMISSION_IP, allowed ? 1 : 0);
        if (connection.send(permission) <= 0 || !allowed) throw TCPException("IP rejected");
        UserMsg user;
        if (connection.receive(user) <= 0 || user.GetType() != RCS_MSG_USER) throw TCPException("Invalid login message");
        char name[20] = {}, password[20] = {};
        if (!user.GetUsername(name) || !user.GetPassword(password) || !name[0]) throw TCPException("Invalid credentials");
        CUserFile users;
        const int power = users.GetUserPower(A2T(name), A2T(password));
        if (power != 0 && power != 1) {
            permission.PacketMsg(RCS_SETUP_MSG_PERMISSION, RCS_PERMISSION_CONTROL, -1);
            connection.send(permission);
            throw TCPException("Login rejected");
        }
        ConnectionData data;
        data.username = A2T(name); data.ip = CString(address.c_str()); data.status = power; data.connection = &connection;
        if (!WndClassList::m_connectDlg->TryAddConnection(data)) {
            permission.PacketMsg(RCS_SETUP_MSG_PERMISSION, RCS_PERMISSION_CONTROL, 2);
            connection.send(permission);
            throw TCPException("Account already connected");
        }
        listed = true;
        permission.PacketMsg(RCS_SETUP_MSG_PERMISSION, RCS_PERMISSION_CONTROL, power);
        if (connection.send(permission) <= 0) throw TCPException("Login reply failed");
        SetupMsg localDesktop;
        localDesktop.PacketMsg(RCS_SETUP_MSG_LOCAL_DESKTOP, static_cast<int>(GetCurrentProcessId()),
                               static_cast<int>(GetCurrentThreadId()));
        if (connection.send(localDesktop) <= 0) throw TCPException("Desktop identity setup failed");
        SetupMsg dimensions;
        dimensions.PacketMsg(RCS_SETUP_MSG_PIXEL, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        if (connection.send(dimensions) <= 0) throw TCPException("Display setup failed");
        images.Start();
        ByteArrayMsg incoming;
        while (connection.receive(incoming) > 0) {
            switch (incoming.GetType()) {
            case RCS_MSG_MOUSE:
                if (WndClassList::m_connectDlg->GetConnectionPower(&connection) == 1) {
                    MouseMsg message; message.SetBuffer(incoming.GetBufferPtr(), incoming.GetSize()); HandleMouse(message, inputState);
                } else { inputState.Release(); }
                break;
            case RCS_MSG_KEYBOARD:
                if (WndClassList::m_connectDlg->GetConnectionPower(&connection) == 1) {
                    KeyboardMsg message; message.SetBuffer(incoming.GetBufferPtr(), incoming.GetSize()); HandleKeyboard(message, inputState);
                } else { inputState.Release(); }
                break;
            case RCS_MSG_SETUP: {
                SetupMsg message; message.SetBuffer(incoming.GetBufferPtr(), incoming.GetSize());
                switch (message.GetStatusType()) {
                case RCS_SETUP_MSG_IMAGE_QUALITY: images.SetImageQuality(message.GetValue1()); break;
                case RCS_SETUP_MSG_FRAME: images.SetFrame(message.GetValue1()); break;
                case RCS_SETUP_MSG_TILE: images.SetTileNumber(message.GetValue1(), message.GetValue2()); break;
                default: break;
                }
                break;
            }
            default: throw TCPException("Unexpected client message");
            }
        }
    } catch (...) {}
    inputState.Release();
    connection.disconnect();
    images.Stop();
    if (listed && WndClassList::m_connectDlg) WndClassList::m_connectDlg->DeleteItemByConnection(&connection);
}
