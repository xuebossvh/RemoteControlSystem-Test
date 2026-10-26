#ifndef __SENDIMAGETHREAD_H__
#define __SENDIMAGETHREAD_H__
#include "../Thread/Thread.h"
#include "../Graphic/ScreenShot.h"
#include "../SocketIPv4/TCPConnection.h"
#include <atomic>
#include <mutex>
#include <queue>
class SendImageThread : public Thread {
public:
    SendImageThread();
    virtual ~SendImageThread();
    void SetConnection(TCPConnection* connection);
    bool SetFrame(int frame);
    bool SetImageQuality(int quality);
    void SetTileNumber(int horizontal, int vertical);
    void Stop();
protected:
    void Run();
private:
    void ClearImages();
    std::queue<ByteArrayMsg*> m_SendQueue;
    ScreenShot m_ScreenShooter;
    std::atomic<int> m_Frame;
    std::atomic<bool> m_stop;
    TCPConnection* m_connection;
    std::mutex m_captureMutex;
};
#endif
