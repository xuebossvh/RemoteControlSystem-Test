#ifndef __TCPCLIENT_H__
#define __TCPCLIENT_H__
#include "TCPConnection.h"
#include "ClientSendQueue.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
const int CONNECT_ERROR = -999;
const int FORBIDDENIP = -998;
const int WRONGMSGTYPE = -997;
class TCPClient : public TCPConnection {
public:
    TCPClient();
    virtual ~TCPClient();
    int Init(const string& ip, TCPSocketAddress::Port port,
        std::queue<ByteArrayMsg*>& sendQueue, std::queue<ByteArrayMsg*>& receiveQueue);
    virtual TCPSocketAddress GetOwnAddress();
    virtual void Start();
    virtual void Close();
    virtual void disconnect() override;
    using TCPConnection::send;
    // Returns bytes accepted into the bounded queue, not bytes acknowledged by
    // the peer. Overflow/failure disconnects explicitly; critical input is never
    // silently dropped or reordered while the connection remains active.
    virtual int send(ByteArrayMsg& message) override;
    using TCPConnection::receive;
    virtual int receive(char* data, int size) override;
    bool PopReceived(ByteArrayMsg*& message);
    bool HasPendingReceived();
    void ClearQueues();
private:
    int ReadPermission(int expectedKind);
    void RecvMessage();
    void SendMessage();
    bool SendPacket(const ClientSendQueue::Packet& packet);
    void StopWorkers(); // The lifecycle mutex is held; workers never call this.
    std::queue<ByteArrayMsg*>* m_sendMsgQueue;
    std::queue<ByteArrayMsg*>* m_recvMsgQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueChanged;
    std::thread m_receiveThread;
    std::thread m_sendThread;
    std::mutex m_lifecycleMutex;
    std::mutex m_outgoingMutex;
    std::condition_variable m_outgoingChanged;
    ClientSendQueue m_outgoing;
    bool m_acceptingOutgoing;
    bool m_streamingIo;
    size_t m_receivedBytes;
    bool m_wsaStarted;
};
#endif
