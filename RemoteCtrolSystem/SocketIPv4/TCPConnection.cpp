#include "TCPConnection.h"
#include "TCPStringBuffer.h"
#include <memory>
TCPConnection::TCPConnection() : m_iFlags(0), m_bConnected(false) {}
TCPConnection::TCPConnection(TCPSocket::Descriptor descriptor)
    : m_socket(descriptor), m_iFlags(0), m_bConnected(true) {}
TCPConnection::~TCPConnection() { disconnect(); }
void TCPConnection::disconnect() { m_bConnected = false; m_socket.Close(); }
void TCPConnection::ShutdownConnection() {
    m_bConnected = false;
    const TCPSocket::Descriptor socket = m_socket;
    if (socket != INVALID_SOCKET) ::shutdown(socket, SD_BOTH);
}
bool TCPConnection::IsConnected() throw() { return m_bConnected.load(); }
void TCPConnection::SetConnected(bool value) throw() { m_bConnected = value; }
void TCPConnection::SetFlags(int flags) throw() { m_iFlags = flags; }
int TCPConnection::SendAll(const char* data, size_t size) {
    if (!data || !size || size > ByteArrayMsg::MAX_IMAGE_BYTES + 21) return -1;
    std::lock_guard<std::mutex> lock(m_sendMutex);
    size_t sent = 0;
    try {
        while (sent < size && IsConnected()) {
            const int count = m_socket.Send(data + sent, size - sent, m_iFlags);
            if (count <= 0) { disconnect(); return -1; }
            sent += count;
        }
    } catch (...) { disconnect(); return -1; }
    return sent == size ? static_cast<int>(sent) : -1;
}
int TCPConnection::send(const TCPBuffer& data) { return SendAll(data.Pointer(), data.Size()); }
int TCPConnection::send(ByteArrayMsg& data) { return SendAll(data.GetBufferPtr(), data.GetSize()); }
int TCPConnection::send(char* data, int size) { return size > 0 ? SendAll(data, size) : -1; }
int TCPConnection::receive(char* data, int size) {
    if (!IsConnected() || !data || size <= 0) return 0;
    try {
        const int count = m_socket.Recv(data, size, m_iFlags);
        if (count <= 0) disconnect();
        return count;
    } catch (...) { disconnect(); return -1; }
}
int TCPConnection::receive(TCPBuffer& data) {
    data.Prepare();
    const int count = receive(data.Pointer(), static_cast<int>(data.Size()));
    if (count >= 0) data.Update(count);
    return count;
}
int TCPConnection::receive(ByteArrayMsg& data) {
    char bytes[DEFAULTBUFSIZE];
    for (;;) {
        const int result = m_framer.Pop(data);
        if (result == 1) return static_cast<int>(data.GetSize());
        if (result < 0) { m_framer.Clear(); disconnect(); return -1; }
        const int count = receive(bytes, sizeof(bytes));
        if (count <= 0) {
            const bool truncated = m_framer.PendingBytes() != 0;
            m_framer.Clear();
            return truncated ? -1 : count;
        }
        if (!m_framer.Append(bytes, count)) { m_framer.Clear(); disconnect(); return -1; }
    }
}
int TCPConnection::receive(std::queue<ByteArrayMsg*>& queue) {
    std::unique_ptr<ByteArrayMsg> message(new ByteArrayMsg);
    const int result = receive(*message);
    if (result > 0) queue.push(message.release());
    return result;
}
int TCPConnection::DealRecvbuffer(char* data, int size, std::queue<ByteArrayMsg*>& queue) {
    if (size < 0 || (size && !m_framer.Append(data, size))) { disconnect(); return -1; }
    for (;;) {
        std::unique_ptr<ByteArrayMsg> message(new ByteArrayMsg);
        const int result = m_framer.Pop(*message);
        if (result < 0) { m_framer.Clear(); disconnect(); return -1; }
        if (!result) return static_cast<int>(m_framer.PendingBytes());
        queue.push(message.release());
    }
}
TCPConnection& TCPConnection::operator<<(const string& text) { TCPStringBuffer data = text; send(data); return *this; }
void TCPConnection::operator>>(string& text) { TCPStringBuffer data; receive(data); text = data; }
TCPSocketAddress TCPConnection::getConnectedAddress() { return m_socket.GetPeerName(); }
TCPSocketAddress TCPConnection::getOwnAddress() { return m_socket.GetSockName(); }
