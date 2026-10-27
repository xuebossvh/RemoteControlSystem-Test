#pragma once
#include "../SocketIPv4/TCPConnection.h"
#include <iphlpapi.h>
#include <vector>
#include <cwchar>

// A loopback address alone is insufficient (port forwarding and other sessions
// exist). Match the reverse live TCP tuple and the advertised process/thread,
// then require the very same interactive Windows session and desktop.
inline bool IsSameInputDesktop(TCPConnection& connection, DWORD process, DWORD thread) {
    if (!process || !thread) return false;
    DWORD ownSession = 0, peerSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) ||
        !ProcessIdToSessionId(process, &peerSession) || ownSession != peerSession) return false;
    HANDLE peerThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread);
    if (!peerThread) return false;
    const bool correctProcess = GetProcessIdOfThread(peerThread) == process;
    CloseHandle(peerThread);
    if (!correctProcess) return false;
    wchar_t localName[256] = {}, peerName[256] = {};
    DWORD bytes = 0;
    BOOL ownInput = FALSE, peerInput = FALSE;
    if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_IO,
            &ownInput, sizeof(ownInput), &bytes) || !ownInput ||
        !GetUserObjectInformationW(GetThreadDesktop(thread), UOI_IO,
            &peerInput, sizeof(peerInput), &bytes) || !peerInput) return false;
    if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
            localName, sizeof(localName), &bytes) ||
        !GetUserObjectInformationW(GetThreadDesktop(thread), UOI_NAME,
            peerName, sizeof(peerName), &bytes) || std::wcscmp(localName, peerName) != 0) return false;
    try {
        const sockaddr_in own = connection.getOwnAddress(), peer = connection.getConnectedAddress();
        ULONG size = 0;
        if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0)
                != ERROR_INSUFFICIENT_BUFFER || size > 16 * 1024 * 1024) return false;
        std::vector<BYTE> storage(size);
        // A single retry handles a connection added while the table is copied.
        DWORD result = GetExtendedTcpTable(storage.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        if (result == ERROR_INSUFFICIENT_BUFFER && size <= 16 * 1024 * 1024) {
            storage.resize(size);
            result = GetExtendedTcpTable(storage.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
        }
        if (result != NO_ERROR) return false;
        const auto table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(storage.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto& row = table->table[i];
            if (row.dwOwningPid == process && row.dwState == MIB_TCP_STATE_ESTAB &&
                row.dwLocalAddr == peer.sin_addr.s_addr && row.dwRemoteAddr == own.sin_addr.s_addr &&
                static_cast<USHORT>(row.dwLocalPort) == peer.sin_port &&
                static_cast<USHORT>(row.dwRemotePort) == own.sin_port) return true;
        }
    } catch (...) {}
    return false;
}
