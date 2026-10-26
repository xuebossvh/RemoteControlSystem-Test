/**************************************************************************
**             Guangzhou ZHIYUAN electronics Co.,LTD
**
**
**                   http://www.electronics.com
**-------------File Info---------------------------------------------------
**File Name:ScreenShot.h
**Latest modified Data:2013-4-3  13:58:53
**Latest Version:v1.0
**Description:
**
**-------------------------------------------------------------------------
**Create by:XuWunJun
**Create date:2013-4-3  13:58:53
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
#include "ImageTile.h"
#include "../Message/ImageMsg.h"
#include <mutex>
#include <queue>
#include <vector>

// Captures the primary desktop. Configuration and capture share the same lock.
class ScreenShot {
public:
    ScreenShot() = default;
    bool Init(int horizontal, int vertical);
    bool ShotScreenImage(std::queue<ByteArrayMsg*>* messages);
    bool SetTileNumber(int horizontal, int vertical);
    bool SetQuality(int quality);
    int GetScreenWidth();
    int GetScreenHeight();
    bool IsPixelChange();
private:
    std::mutex mutex_;
    int columns_ = 15, rows_ = 15, width_ = 0, height_ = 0, quality_ = 60;
    bool pixelChanged_ = false;
    std::vector<ImageTile> previous_;
};
