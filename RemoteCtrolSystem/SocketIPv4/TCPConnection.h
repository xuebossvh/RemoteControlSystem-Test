#ifndef __TCPCONNECTION_H__
#define __TCPCONNECTION_H__
#include "TCPSocket.h"
#include "../Message/MessageFramer.h"
#include <atomic>
#include <mutex>
#include <queue>
const int DEFAULTBUFSIZE = 64 * 1024;
class TCPConnection {
public:
    TCPConnection();
    virtual ~TCPConnection();
    virtual void disconnect();
    virtual int send(const TCPBuffer& data);
    virtual int send(ByteArrayMsg& data);
    virtual int send(char* data, int size);
    virtual int receive(TCPBuffer& data);
    virtual int receive(ByteArrayMsg& data);
    virtual int receive(char* data, int size);
    virtual int receive(std::queue<ByteArrayMsg*>& queue);
    int DealRecvbuffer(char* data, int size, std::queue<ByteArrayMsg*>& queue);
    virtual TCPConnection& operator<<(const string& text);
    virtual void operator>>(string& text);
    virtual TCPSocketAddress getConnectedAddress();
    virtual TCPSocketAddress getOwnAddress();
    virtual bool IsConnected() throw();
    void SetFlags(int flags) throw();
    int GetFlags() throw() { return m_iFlags; }
protected:
    void SetConnected(bool value) throw();
    // Interrupt duplex I/O without releasing its descriptor. A threaded owner
    // can join workers before disconnect() closes/recycles the socket handle.
    void ShutdownConnection();
    void ResetReceiveBuffer() { m_framer.Clear(); }
    explicit TCPConnection(TCPSocket::Descriptor descriptor);
    TCPSocket m_socket;
    friend class TCPServer;
private:
    int SendAll(const char* data, size_t size);
    int m_iFlags;
    std::atomic<bool> m_bConnected;
    std::mutex m_sendMutex;
    MessageFramer m_framer;
};
#endif
