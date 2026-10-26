#include "TCPClient.h"
#include "../Message/SetupMsg.h"
#include <memory>
TCPClient::TCPClient() : m_sendMsgQueue(NULL), m_recvMsgQueue(NULL), m_receivedBytes(0), m_wsaStarted(false) {}
TCPClient::~TCPClient() { Close(); if (m_wsaStarted) WSACleanup(); }
int TCPClient::Init(const string& ip, TCPSocketAddress::Port port,
    std::queue<ByteArrayMsg*>& sendQueue, std::queue<ByteArrayMsg*>& receiveQueue) {
    // The caller queues the login message immediately before Init.
    disconnect();
    m_queueChanged.notify_all();
    if (m_receiveThread.joinable()) m_receiveThread.join();
    ResetReceiveBuffer();
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_recvMsgQueue) while (!m_recvMsgQueue->empty()) { delete m_recvMsgQueue->front(); m_recvMsgQueue->pop(); }
        m_sendMsgQueue = &sendQueue;
        m_recvMsgQueue = &receiveQueue;
        while (!receiveQueue.empty()) { delete receiveQueue.front(); receiveQueue.pop(); }
        m_receivedBytes = 0;
    }
    try {
        if (!m_wsaStarted) {
            WSADATA data;
            if (WSAStartup(MAKEWORD(2, 2), &data)) return CONNECT_ERROR;
            m_wsaStarted = true;
        }
        m_socket.Open(TCPSocket::TCP);
        m_socket.Connect(TCPSocketAddress(ip, port));
        SetConnected(true);
        DWORD timeout = 5000;
        setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        const int ipPermission = ReadPermission(RCS_PERMISSION_IP);
        if (ipPermission != 1) { Close(); return ipPermission == 0 ? FORBIDDENIP : ipPermission; }
        std::unique_ptr<ByteArrayMsg> login;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!sendQueue.empty()) { login.reset(sendQueue.front()); sendQueue.pop(); }
        }
        if (!login || login->GetType() != RCS_MSG_USER || send(*login) <= 0) { Close(); return CONNECT_ERROR; }
        const int power = ReadPermission(RCS_PERMISSION_CONTROL);
        if (power != 0 && power != 1) { Close(); return power; }
        timeout = 0;
        setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        return power;
    } catch (...) { Close(); return CONNECT_ERROR; }
}
int TCPClient::ReadPermission(int expectedKind) {
    SetupMsg message;
    if (receive(message) <= 0) return CONNECT_ERROR;
    if (message.GetType() != RCS_MSG_SETUP || message.GetStatusType() != RCS_SETUP_MSG_PERMISSION ||
        message.GetValue1() != expectedKind) return WRONGMSGTYPE;
    return message.GetValue2();
}
TCPSocketAddress TCPClient::GetOwnAddress() { return m_socket.GetSockName(); }
void TCPClient::Start() {
    if (!m_receiveThread.joinable() && IsConnected()) m_receiveThread = std::thread(&TCPClient::RecvMessage, this);
}
void TCPClient::RecvMessage() {
    try {
        while (IsConnected()) {
            std::unique_ptr<ByteArrayMsg> message(new ByteArrayMsg);
            if (receive(*message) <= 0) break;
            const size_t length = message->GetSize();
            std::unique_lock<std::mutex> lock(m_queueMutex);
            // Apply backpressure instead of growing an unbounded GUI queue.
            m_queueChanged.wait(lock, [this, length] {
                return !IsConnected() || (m_recvMsgQueue && m_recvMsgQueue->size() < 2048 && m_receivedBytes + length <= 32u * 1024u * 1024u);
            });
            if (!IsConnected()) break;
            m_receivedBytes += length;
            m_recvMsgQueue->push(message.release());
        }
    } catch (...) {}
    disconnect();
    m_queueChanged.notify_all();
}
bool TCPClient::PopReceived(ByteArrayMsg*& message) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    message = NULL;
    if (!m_recvMsgQueue || m_recvMsgQueue->empty()) return false;
    message = m_recvMsgQueue->front(); m_recvMsgQueue->pop();
    m_receivedBytes -= message->GetSize();
    m_queueChanged.notify_one();
    return true;
}
bool TCPClient::HasPendingReceived() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_recvMsgQueue && !m_recvMsgQueue->empty();
}
void TCPClient::ClearQueues() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    std::queue<ByteArrayMsg*>* queues[] = {m_sendMsgQueue, m_recvMsgQueue};
    for (auto queue : queues) if (queue) while (!queue->empty()) { delete queue->front(); queue->pop(); }
    m_receivedBytes = 0;
}
void TCPClient::Close() {
    disconnect();
    m_queueChanged.notify_all();
    if (m_receiveThread.joinable()) m_receiveThread.join();
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        std::queue<ByteArrayMsg*>* queues[] = {m_sendMsgQueue, m_recvMsgQueue};
        for (auto queue : queues) if (queue) while (!queue->empty()) { delete queue->front(); queue->pop(); }
        m_receivedBytes = 0;
        m_sendMsgQueue = NULL;
        m_recvMsgQueue = NULL;
    }
    ResetReceiveBuffer();
}
