/**************************************************************************
**             Guangzhou ZHIYUAN electronics Co.,LTD
**
**
**                   http://www.electronics.com
**-------------File Info---------------------------------------------------
**File Name:ScreenDisplay.h
**Latest modified Data:2013-4-3  9:39:09
**Latest Version:v1.0
**Description:
**
**-------------------------------------------------------------------------
**Create by:XuWunJun
**Create date:2013-4-3  9:39:09
**Version:v1.0
**Description:
**
**
**-------------------------------------------------------------------------
**Modified by:
**Modified date:
**Version:
**Description:
**************************************************************************/
// Modernized image implementation: UTF-8, C++14, Windows GDI/GDI+.
#pragma once
#include "../Message/ImageMsg.h"
#include "GdiImage.h"

// Shared, window-independent geometry for both drawing and input. A single
// scale is applied to both axes; integer raster dimensions may differ by <1 px.
class ScreenViewGeometry {
public:
    bool SetSourceSize(int width, int height);
    bool SetViewportSize(int width, int height);
    void Fit();
    bool Zoom(double factor);
    RECT GetDisplayRect() const { return displayRect_; }
    bool MapWindowPointToServer(int x, int y, int& serverX, int& serverY) const;
private:
    void Recalculate();
    int sourceWidth_ = 0, sourceHeight_ = 0;
    int viewportWidth_ = 0, viewportHeight_ = 0;
    double zoom_ = 1.0;
    RECT displayRect_ = {};
};

// Used on the UI thread; owns a persistent full-size image and a GDI back buffer.
class ScreenDisplay {
public:
    ScreenDisplay() = default;
    ~ScreenDisplay();
    ScreenDisplay(const ScreenDisplay&) = delete;
    ScreenDisplay& operator=(const ScreenDisplay&) = delete;
    bool Initialize(HWND window, int windowWidth, int windowHeight, int serverWidth, int serverHeight);
    bool SetSize(int width, int height);
    // WM_PAINT / WM_PRINTCLIENT share this renderer. The caller owns targetDC.
    bool Render(HDC targetDC);
    // Schedule a paint; never draw around the window's WM_PAINT lifecycle.
    void Display();
    bool RecvImageMsg(ByteArrayMsg* message); // Consumes/deletes message on every path; false rejects bad data.
    int GetDisplayWidth() const { const RECT r = view_.GetDisplayRect(); return r.right - r.left; }
    int GetDisplayHeight() const { const RECT r = view_.GetDisplayRect(); return r.bottom - r.top; }
    RECT GetDisplayRect() const { return view_.GetDisplayRect(); }
    // x/y are client coordinates of the display static. Black bars are rejected.
    bool MapWindowPointToServer(int x, int y, int& serverX, int& serverY) const;
    int GetSeverWidth() { return serverWidth_; }
    int GetServerHeight() { return serverHeight_; }
    int GetWinWidth() { return windowWidth_; }
    int GetWinHeigth() { return windowHeight_; }
    void SetServerSize(int width, int height);
    GdiImage GetDisplayImage() { return frame_; }
    bool ScaleUp();
    bool ScaleDown();
    void Scale2LocalSize();
    bool ReflashDC();
private:
    void ReleaseDevice();
    HWND window_ = nullptr;
    HDC memoryDC_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ originalBitmap_ = nullptr;
    GdiImage frame_;
    int windowWidth_ = 0, windowHeight_ = 0;
    int serverWidth_ = 0, serverHeight_ = 0;
    ScreenViewGeometry view_;
};
