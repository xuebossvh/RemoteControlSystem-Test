#ifndef __TCPCLIENT_H__
#define __TCPCLIENT_H__
#include "TCPConnection.h"
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
    bool PopReceived(ByteArrayMsg*& message);
    bool HasPendingReceived();
    void ClearQueues();
private:
    int ReadPermission(int expectedKind);
    void RecvMessage();
    std::queue<ByteArrayMsg*>* m_sendMsgQueue;
    std::queue<ByteArrayMsg*>* m_recvMsgQueue;
    std::mutex m_queueMutex;
    std::condition_variable m_queueChanged;
    std::thread m_receiveThread;
    size_t m_receivedBytes;
    bool m_wsaStarted;
};
#endif
