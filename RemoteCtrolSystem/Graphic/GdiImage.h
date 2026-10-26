#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstddef>
#include <vector>

// Native Windows image support used by this application, not a CxImage replacement.
enum GdiImageFormat { GDI_IMAGE_BMP = 1, GDI_IMAGE_GIF = 2, GDI_IMAGE_JPG = 3,
                      GDI_IMAGE_PNG = 4, GDI_IMAGE_TIF = 6 };

// Owns top-down 24-bit BGR pixels. Rows are padded to a multiple of four bytes.
// Copies are deep copies; no GDI/GDI+ object or stream survives an operation.
class GdiImage {
public:
    bool Create(int width, int height, int bpp = 24, DWORD format = GDI_IMAGE_JPG);
    bool CreateFromHBITMAP(HBITMAP bitmap);
    bool CreateFromArray(const BYTE* pixels, int width, int height, int bpp,
                         std::size_t stride, bool topDown = true);
    bool Decode(const BYTE* bytes, std::size_t size, DWORD format = GDI_IMAGE_JPG);
    bool Encode(std::vector<BYTE>& bytes, DWORD format = GDI_IMAGE_JPG) const;
    bool Encode(BYTE*& bytes, long& size, DWORD format = GDI_IMAGE_JPG) const;
    static void FreeMemory(void* bytes);
    bool Save(const wchar_t* filename, DWORD format) const;
    bool MixFrom(const GdiImage& image, int x, int y);
    bool Stretch(HDC dc, int x, int y, int width, int height) const;
    void SetJpegQuality(int quality);
    int GetJpegQuality() const { return quality_; }
    BYTE* GetBits() { return pixels_.empty() ? nullptr : pixels_.data(); }
    const BYTE* GetBits() const { return pixels_.empty() ? nullptr : pixels_.data(); }
    std::size_t GetStride() const { return stride_; }
    int GetWidth() const { return width_; }
    int GetHeight() const { return height_; }
    bool IsValid() const { return !pixels_.empty(); }
private:
    int width_ = 0, height_ = 0, quality_ = 60;
    std::size_t stride_ = 0;
    std::vector<BYTE> pixels_;
};
