/**************************************************************************
**             Guangzhou ZHIYUAN electronics Co.,LTD
**
**
**                   http://www.electronics.com
**-------------File Info---------------------------------------------------
**File Name:ImageTile.h
**Latest modified Data:2013-4-3  14:01:22
**Latest Version:v1.0
**Description:
**
**-------------------------------------------------------------------------
**Create by:XuWunJun
**Create date:2013-4-3  14:01:22
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
#include "GdiImage.h"
#include <vector>

// A top-down, tightly packed BGR tile. Unlike GdiImage, its rows have no padding.
class ImageTile {
public:
    bool CopyFrom(const GdiImage& image, RECT rect);
    bool SetImageTile(char* pixels, RECT screen, RECT tile);
    bool SetImageTileReverse(char* pixels, RECT screen, RECT tile);
    bool ImageBits(char* destination) const;
    char* GetBitsPtr() { return pixels_.empty() ? nullptr : reinterpret_cast<char*>(pixels_.data()); }
    const char* GetBitsPtr() const { return pixels_.empty() ? nullptr : reinterpret_cast<const char*>(pixels_.data()); }
    RECT Rect() const { return rect_; }
    std::size_t Size() const { return pixels_.size(); }
    bool Matches(const ImageTile& other) const;
    friend void SwapImageTile(ImageTile& first, ImageTile& second);
private:
    bool Copy(const BYTE* pixels, int width, int height, std::size_t stride, RECT rect, bool topDown);
    RECT rect_ = {};
    std::vector<BYTE> pixels_;
};
