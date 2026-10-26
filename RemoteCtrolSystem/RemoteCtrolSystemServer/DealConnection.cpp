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

DealConnection::~DealConnection() {}
namespace {
void HandleMouse(MouseMsg& message) {
    const int x = message.GetMouseX(), y = message.GetMouseY();
    if (x < 0 || y < 0 || x >= GetSystemMetrics(SM_CXSCREEN) || y >= GetSystemMetrics(SM_CYSCREEN)) return;
    switch (message.GetMouseStatus()) {
    case 0: MouseKeybdEvent::Move(x, y); break;
    case RCS_MOUSE_LEFT_DOWN: MouseKeybdEvent::LeftClickDown(x, y); break;
    case RCS_MOUSE_LEFT_UP: MouseKeybdEvent::LeftClickUp(x, y); break;
    case RCS_MOUSE_RIGHT_DOWN: MouseKeybdEvent::RightClickDown(x, y); break;
    case RCS_MOUSE_RIGHT_UP: MouseKeybdEvent::RightClickUp(x, y); break;
    case RCS_MOUSE_LEFT_DBLCLK: MouseKeybdEvent::LeftDbClick(x, y); break;
    case RCS_MOUSE_RIGHT_DBLCLK: MouseKeybdEvent::RightDbClick(x, y); break;
    default: break; // The legacy wheel packet does not contain an unambiguous delta.
    }
}
void HandleKeyboard(KeyboardMsg& message) {
    const int key = message.GetKey(), state = message.GetKeyStatus();
    if (key == RCS_KEY_TASKMGR) { WinExec("taskmgr.exe", SW_SHOW); return; }
    if (key < 0 || key > 255) return;
    if (state == RCS_KEY_DOWN) MouseKeybdEvent::KeyDown(key, FALSE);
    else if (state == RCS_KEY_UP) MouseKeybdEvent::KeyUp(key, FALSE);
}
}
void DealConnection::Run() {
    USES_CONVERSION;
    bool listed = false;
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
        SetupMsg dimensions;
        dimensions.PacketMsg(RCS_SETUP_MSG_PIXEL, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        if (connection.send(dimensions) <= 0) throw TCPException("Display setup failed");
        images.Start();
        ByteArrayMsg incoming;
        while (connection.receive(incoming) > 0) {
            switch (incoming.GetType()) {
            case RCS_MSG_MOUSE:
                if (WndClassList::m_connectDlg->GetConnectionPower(&connection) == 1) {
                    MouseMsg message; message.SetBuffer(incoming.GetBufferPtr(), incoming.GetSize()); HandleMouse(message);
                }
                break;
            case RCS_MSG_KEYBOARD:
                if (WndClassList::m_connectDlg->GetConnectionPower(&connection) == 1) {
                    KeyboardMsg message; message.SetBuffer(incoming.GetBufferPtr(), incoming.GetSize()); HandleKeyboard(message);
                }
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
    connection.disconnect();
    images.Stop();
    if (listed && WndClassList::m_connectDlg) WndClassList::m_connectDlg->DeleteItemByConnection(&connection);
}
