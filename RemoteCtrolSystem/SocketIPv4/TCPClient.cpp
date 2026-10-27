#include "TCPClient.h"
#include "../Message/SetupMsg.h"
#include <chrono>
#include <memory>

namespace {
int WaitReady(TCPSocket::Descriptor socket, bool write) {
    if (socket == INVALID_SOCKET) return SOCKET_ERROR;
    fd_set descriptors;
    FD_ZERO(&descriptors); FD_SET(socket, &descriptors);
    timeval timeout = {0, 100000};
    return ::select(0, write ? nullptr : &descriptors, write ? &descriptors : nullptr, nullptr, &timeout);
}
}

TCPClient::TCPClient() : m_sendMsgQueue(NULL), m_recvMsgQueue(NULL),
    m_acceptingOutgoing(false), m_streamingIo(false), m_receivedBytes(0), m_wsaStarted(false) {}
TCPClient::~TCPClient() { Close(); if (m_wsaStarted) WSACleanup(); }

int TCPClient::Init(const string& ip, TCPSocketAddress::Port port,
    std::queue<ByteArrayMsg*>& sendQueue, std::queue<ByteArrayMsg*>& receiveQueue) {
    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    // Preserve the new login before clearing an earlier session: callers place
    // it in the legacy queue before Init, and can reuse the same queue object.
    std::unique_ptr<ByteArrayMsg> login;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!sendQueue.empty()) { login.reset(sendQueue.front()); sendQueue.pop(); }
    }
    StopWorkers();
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_sendMsgQueue = &sendQueue;
        m_recvMsgQueue = &receiveQueue;
        for (auto queue : {&sendQueue, &receiveQueue})
            while (!queue->empty()) { delete queue->front(); queue->pop(); }
        m_receivedBytes = 0;
    }
    try {
        if (!login || login->GetType() != RCS_MSG_USER) { StopWorkers(); return CONNECT_ERROR; }
        if (!m_wsaStarted) {
            WSADATA data;
            if (WSAStartup(MAKEWORD(2, 2), &data)) { StopWorkers(); return CONNECT_ERROR; }
            m_wsaStarted = true;
        }
        m_socket.Open(TCPSocket::TCP);
        m_socket.Connect(TCPSocketAddress(ip, port));
        SetConnected(true);
        if (m_socket.SetNodelay(true) == SOCKET_ERROR) throw TCPException("TCP_NODELAY failed");
        DWORD timeout = 5000;
        if (setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR)
            throw TCPException("Login receive timeout failed");
        // A stalled peer must not keep the dedicated sender blocked forever.
        // UI producers never wait for this timeout, and Close uses shutdown to
        // interrupt the in-flight send before joining the worker.
        timeout = 2000;
        if (setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR)
            throw TCPException("Input send timeout failed");
        const int ipPermission = ReadPermission(RCS_PERMISSION_IP);
        if (ipPermission != 1) { StopWorkers(); return ipPermission == 0 ? FORBIDDENIP : ipPermission; }
        // Authentication is synchronous and completes before either worker is
        // started. Runtime UI messages use the asynchronous override below.
        if (TCPConnection::send(*login) <= 0) { StopWorkers(); return CONNECT_ERROR; }
        const int power = ReadPermission(RCS_PERMISSION_CONTROL);
        if (power != 0 && power != 1) { StopWorkers(); return power; }
        timeout = 0;
        if (setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR)
            throw TCPException("Streaming receive timeout failed");
        return power;
    } catch (...) { StopWorkers(); return CONNECT_ERROR; }
}
int TCPClient::ReadPermission(int expectedKind) {
    SetupMsg message;
    if (receive(message) <= 0) return CONNECT_ERROR;
    if (message.GetType() != RCS_MSG_SETUP || message.GetStatusType() != RCS_SETUP_MSG_PERMISSION ||
        message.GetValue1() != expectedKind) return WRONGMSGTYPE;
    return message.GetValue2();
}
TCPSocketAddress TCPClient::GetOwnAddress() { return getOwnAddress(); }

void TCPClient::Start() {
    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    if (!IsConnected() || m_sendThread.joinable() || m_receiveThread.joinable()) return;
    u_long nonblocking = 1;
    if (ioctlsocket(m_socket, FIONBIO, &nonblocking) == SOCKET_ERROR) { StopWorkers(); return; }
    m_streamingIo = true; // Written before worker creation, reset only after joining them.
    {
        std::lock_guard<std::mutex> lock(m_outgoingMutex);
        m_outgoing.Clear();
        m_acceptingOutgoing = true;
    }
    try {
        m_sendThread = std::thread(&TCPClient::SendMessage, this);
        m_receiveThread = std::thread(&TCPClient::RecvMessage, this);
    } catch (...) { StopWorkers(); }
}
int TCPClient::send(ByteArrayMsg& message) {
    const size_t length = message.GetSize();
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(m_outgoingMutex);
        if (!m_acceptingOutgoing || !IsConnected()) return -1;
        accepted = m_outgoing.Push(message.GetBufferPtr(), length);
        if (!accepted) {
            // Mark and close this generation while still holding the queue
            // lock. A delayed producer must never close a later Init session.
            m_acceptingOutgoing = false;
            ShutdownConnection();
        }
    }
    if (!accepted) {
        // Bounded memory, nonblocking UI and lossless unlimited key bursts
        // cannot all be guaranteed. Explicitly fail this session instead of
        // silently losing a button/key-up while pretending it is still usable.
        m_outgoingChanged.notify_all();
        m_queueChanged.notify_all();
        return -1;
    }
    m_outgoingChanged.notify_one();
    return static_cast<int>(length);
}
void TCPClient::SendMessage() {
    using Clock = std::chrono::steady_clock;
    auto nextMovement = (Clock::time_point::min)();
    try {
        for (;;) {
            ClientSendQueue::Packet packet;
            {
                std::unique_lock<std::mutex> lock(m_outgoingMutex);
                m_outgoingChanged.wait(lock, [this] {
                    return !m_acceptingOutgoing || !IsConnected() || !m_outgoing.Empty();
                });
                if (!m_acceptingOutgoing || !IsConnected()) break;
                if (m_outgoing.FrontIsMovement() && m_outgoing.Size() == 1 && Clock::now() < nextMovement) {
                    // Never discard the final position. Adjacent moves replace
                    // this pending one until the fixed 8 ms deadline. A button,
                    // key or setting behind it wakes us immediately, preserving
                    // the move-before-click ordering without delaying controls.
                    m_outgoingChanged.wait_until(lock, nextMovement, [this] {
                        return !m_acceptingOutgoing || !IsConnected() || m_outgoing.Size() > 1 ||
                               !m_outgoing.FrontIsMovement();
                    });
                }
                if (!m_acceptingOutgoing || !IsConnected()) break;
                if (!m_outgoing.Pop(packet)) continue;
            }
            // No queue or lifecycle lock is held across network I/O.
            if (!SendPacket(packet)) break;
            if (packet.movement) nextMovement = Clock::now() + std::chrono::milliseconds(8);
        }
    } catch (...) {}
    disconnect();
}
bool TCPClient::SendPacket(const ClientSendQueue::Packet& packet) {
    size_t offset = 0;
    auto progress = std::chrono::steady_clock::now();
    while (IsConnected() && offset < packet.size) {
        const int sent = m_socket.Send(packet.bytes.data() + offset, packet.size - offset);
        if (sent > 0) {
            offset += static_cast<size_t>(sent);
            progress = std::chrono::steady_clock::now();
            continue;
        }
        if (sent != SOCKET_ERROR || WSAGetLastError() != WSAEWOULDBLOCK) return false;
        if (std::chrono::steady_clock::now() - progress >= std::chrono::seconds(2)) return false;
        if (WaitReady(m_socket, true) == SOCKET_ERROR) return false;
    }
    return offset == packet.size;
}
int TCPClient::receive(char* data, int size) {
    if (!m_streamingIo) return TCPConnection::receive(data, size); // Login retains its receive deadline.
    if (!data || size <= 0) return 0;
    try {
        while (IsConnected()) {
            const int ready = WaitReady(m_socket, false);
            if (!IsConnected()) return 0;
            if (ready == SOCKET_ERROR) { disconnect(); return -1; }
            if (!ready) continue;
            const int received = m_socket.Recv(data, size);
            if (received > 0) return received;
            if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
            disconnect(); return received;
        }
    } catch (...) { disconnect(); return -1; }
    return 0;
}
void TCPClient::RecvMessage() {
    try {
        while (IsConnected()) {
            std::unique_ptr<ByteArrayMsg> message(new ByteArrayMsg);
            if (receive(*message) <= 0) break;
            const size_t length = message->GetSize();
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueChanged.wait(lock, [this, length] {
                return !IsConnected() || (m_recvMsgQueue && m_recvMsgQueue->size() < 2048 && m_receivedBytes + length <= 32u * 1024u * 1024u);
            });
            if (!IsConnected()) break;
            m_receivedBytes += length;
            m_recvMsgQueue->push(message.get());
            message.release();
        }
    } catch (...) {}
    disconnect();
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
    m_queueChanged.notify_all();
}
void TCPClient::disconnect() {
    {
        std::lock_guard<std::mutex> lock(m_outgoingMutex);
        m_acceptingOutgoing = false;
        // Keep the handle alive until StopWorkers joins both I/O threads. This
        // prevents a concurrent send/recv from observing a recycled SOCKET.
        ShutdownConnection();
    }
    m_outgoingChanged.notify_all();
    m_queueChanged.notify_all();
}
void TCPClient::StopWorkers() {
    disconnect();
    if (m_sendThread.joinable()) m_sendThread.join();
    if (m_receiveThread.joinable()) m_receiveThread.join();
    m_streamingIo = false;
    {
        std::lock_guard<std::mutex> lock(m_outgoingMutex);
        m_outgoing.Clear();
        TCPConnection::disconnect();
    }
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
void TCPClient::Close() {
    std::lock_guard<std::mutex> lifecycle(m_lifecycleMutex);
    StopWorkers();
}
