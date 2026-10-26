#include "StdAfx.h"
#include "SendImageThread.h"
#include "../Message/SetupMsg.h"
#include <memory>
SendImageThread::SendImageThread() : m_Frame(10), m_stop(false), m_connection(NULL) { m_ScreenShooter.Init(15, 15); }
SendImageThread::~SendImageThread() { Stop(); ClearImages(); }
void SendImageThread::Stop() {
    m_stop = true;
    if (m_connection) m_connection->disconnect();
    WaitForEnd();
}
void SendImageThread::ClearImages() {
    while (!m_SendQueue.empty()) { delete m_SendQueue.front(); m_SendQueue.pop(); }
}
void SendImageThread::SetConnection(TCPConnection* connection) { m_connection = connection; }
bool SendImageThread::SetFrame(int frame) {
    if (frame < 1 || frame > 30) return false;
    m_Frame = frame; return true;
}
bool SendImageThread::SetImageQuality(int quality) {
    if (quality < 1 || quality > 100) return false;
    std::lock_guard<std::mutex> lock(m_captureMutex);
    return m_ScreenShooter.SetQuality(quality);
}
void SendImageThread::SetTileNumber(int horizontal, int vertical) {
    if (horizontal < 1 || vertical < 1 || horizontal > 32 || vertical > 32) return;
    std::lock_guard<std::mutex> lock(m_captureMutex);
    m_ScreenShooter.Init(horizontal, vertical);
}
void SendImageThread::Run() {
    unsigned captureFailures = 0;
    try {
        while (!m_stop && m_connection && m_connection->IsConnected()) {
            const DWORD frameStart = GetTickCount();
            bool changed = false, captured = false;
            int width = 0, height = 0;
            {
                std::lock_guard<std::mutex> lock(m_captureMutex);
                captured = m_ScreenShooter.ShotScreenImage(&m_SendQueue);
                changed = m_ScreenShooter.IsPixelChange();
                width = m_ScreenShooter.GetScreenWidth(); height = m_ScreenShooter.GetScreenHeight();
            }
            if (!captured) {
                ClearImages();
                if (++captureFailures == 1 || captureFailures % 50 == 0)
                    OutputDebugStringW(L"RemoteControlSystem: capture unavailable; retaining client frame and retrying.\n");
                for (int wait = 0; wait < 40 && !m_stop && m_connection->IsConnected(); ++wait) Sleep(5);
                continue;
            }
            captureFailures = 0;
            if (changed) {
                SetupMsg dimensions;
                dimensions.PacketMsg(RCS_SETUP_MSG_PIXEL, width, height);
                if (m_connection->send(dimensions) <= 0) break;
            }
            while (!m_SendQueue.empty() && !m_stop && m_connection->IsConnected()) {
                std::unique_ptr<ByteArrayMsg> message(m_SendQueue.front()); m_SendQueue.pop();
                if (m_connection->send(*message) <= 0) break;
            }
            ClearImages();
            const DWORD interval = 1000 / m_Frame.load();
            while (!m_stop && m_connection->IsConnected() && GetTickCount() - frameStart < interval) Sleep(5);
        }
    } catch (...) {}
    ClearImages();
    if (m_connection) m_connection->disconnect();
}
