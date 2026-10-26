#include "GdiImage.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace {
const std::size_t kMaxImageBytes = 256u * 1024u * 1024u;
const std::size_t kMaxEncodedBytes = 64u * 1024u * 1024u;
class GdiPlusRuntime {
public:
    GdiPlusRuntime() {
        Gdiplus::GdiplusStartupInput input;
        ready = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }
    ~GdiPlusRuntime() { if (ready) Gdiplus::GdiplusShutdown(token); }
    bool ready = false;
private:
    ULONG_PTR token = 0;
};
bool Ready() { static GdiPlusRuntime runtime; return runtime.ready; }
struct StreamReleaser { void operator()(IStream* p) const { if (p) p->Release(); } };
using StreamPtr = std::unique_ptr<IStream, StreamReleaser>;

bool Encoder(DWORD format, CLSID& clsid) {
    const wchar_t* mime = nullptr;
    switch (format) {
    case GDI_IMAGE_BMP: mime = L"image/bmp"; break;
    case GDI_IMAGE_GIF: mime = L"image/gif"; break;
    case GDI_IMAGE_JPG: mime = L"image/jpeg"; break;
    case GDI_IMAGE_PNG: mime = L"image/png"; break;
    case GDI_IMAGE_TIF: mime = L"image/tiff"; break;
    default: return false;
    }
    UINT count = 0, size = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0)
        return false;
    std::vector<BYTE> storage(size);
    auto codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, size, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) { clsid = codecs[i].Clsid; return true; }
    }
    return false;
}
Gdiplus::EncoderParameters QualityParameters(ULONG& quality) {
    Gdiplus::EncoderParameters params = {};
    params.Count = 1;
    params.Parameter[0].Guid = Gdiplus::EncoderQuality;
    params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    params.Parameter[0].Value = &quality;
    return params;
}
}

bool GdiImage::Create(int width, int height, int bpp, DWORD) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || bpp != 24)
        return false;
    const std::size_t stride = (static_cast<std::size_t>(width) * 3 + 3) & ~std::size_t(3);
    if (stride > kMaxImageBytes / static_cast<std::size_t>(height)) return false;
    try {
        std::vector<BYTE> pixels(stride * height, 0);
        pixels_.swap(pixels);
        width_ = width; height_ = height; stride_ = stride;
        return true;
    } catch (const std::bad_alloc&) { return false; }
}

bool GdiImage::CreateFromHBITMAP(HBITMAP bitmap) {
    BITMAP info = {};
    if (!bitmap || GetObject(bitmap, sizeof(info), &info) == 0) return false;
    GdiImage result;
    if (!result.Create(info.bmWidth, info.bmHeight)) return false;
    BITMAPINFO dib = {};
    dib.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dib.bmiHeader.biWidth = result.width_;
    dib.bmiHeader.biHeight = -result.height_; // Request top-down BGR, including for a 32-bit desktop.
    dib.bmiHeader.biPlanes = 1;
    dib.bmiHeader.biBitCount = 24;
    dib.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    const int rows = GetDIBits(dc, bitmap, 0, result.height_, result.GetBits(), &dib, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (rows != result.height_) return false;
    result.quality_ = quality_;
    *this = std::move(result);
    return true;
}

bool GdiImage::CreateFromArray(const BYTE* pixels, int width, int height, int bpp,
                              std::size_t stride, bool topDown) {
    if (!pixels || width <= 0 || height <= 0 || bpp != 24 ||
        stride < static_cast<std::size_t>(width) * 3 ||
        stride > kMaxImageBytes / static_cast<std::size_t>(height)) return false;
    GdiImage result;
    if (!result.Create(width, height)) return false;
    for (int y = 0; y < height; ++y) {
        const int sourceY = topDown ? y : height - 1 - y;
        std::memcpy(result.GetBits() + y * result.stride_, pixels + sourceY * stride,
                    static_cast<std::size_t>(width) * 3);
    }
    result.quality_ = quality_;
    *this = std::move(result);
    return true;
}

bool GdiImage::Decode(const BYTE* bytes, std::size_t size, DWORD format) {
    if (!bytes || size == 0 || size > kMaxEncodedBytes || !Ready()) return false;
    // The network protocol explicitly carries JPEG tiles. Reject another format early.
    if (format == GDI_IMAGE_JPG && (size < 2 || bytes[0] != 0xff || bytes[1] != 0xd8))
        return false;
    IStream* raw = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &raw))) return false;
    StreamPtr stream(raw);
    ULONG written = 0;
    if (FAILED(stream->Write(bytes, static_cast<ULONG>(size), &written)) || written != size)
        return false;
    LARGE_INTEGER beginning = {};
    if (FAILED(stream->Seek(beginning, STREAM_SEEK_SET, nullptr))) return false;
    Gdiplus::Bitmap bitmap(stream.get(), FALSE);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) return false;
    GdiImage result;
    if (!result.Create(static_cast<int>(bitmap.GetWidth()), static_cast<int>(bitmap.GetHeight())))
        return false;
    Gdiplus::Rect rect(0, 0, result.width_, result.height_);
    Gdiplus::BitmapData locked = {};
    if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat24bppRGB, &locked) != Gdiplus::Ok)
        return false;
    const std::ptrdiff_t pitch = locked.Stride;
    const std::size_t rowSize = static_cast<std::size_t>(result.width_) * 3;
    if (!locked.Scan0 || (pitch >= 0 ? pitch : -pitch) < static_cast<std::ptrdiff_t>(rowSize)) {
        bitmap.UnlockBits(&locked); return false;
    }
    for (int y = 0; y < result.height_; ++y)
        std::memcpy(result.GetBits() + y * result.stride_,
                    static_cast<const BYTE*>(locked.Scan0) + y * pitch, rowSize);
    const auto status = bitmap.UnlockBits(&locked);
    if (status != Gdiplus::Ok) return false;
    result.quality_ = quality_;
    *this = std::move(result);
    return true;
}

bool GdiImage::Encode(std::vector<BYTE>& bytes, DWORD format) const {
    bytes.clear();
    if (!IsValid() || !Ready()) return false;
    try {
        CLSID clsid;
        if (!Encoder(format, clsid)) return false;
        Gdiplus::Bitmap bitmap(width_, height_, static_cast<INT>(stride_), PixelFormat24bppRGB,
                               const_cast<BYTE*>(GetBits()));
        if (bitmap.GetLastStatus() != Gdiplus::Ok) return false;
        IStream* raw = nullptr;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &raw))) return false;
        StreamPtr stream(raw);
        ULONG quality = quality_;
        auto params = QualityParameters(quality);
        if (bitmap.Save(stream.get(), &clsid, format == GDI_IMAGE_JPG ? &params : nullptr) != Gdiplus::Ok)
            return false;
        STATSTG stat = {};
        if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0 ||
            stat.cbSize.QuadPart > kMaxEncodedBytes) return false;
        LARGE_INTEGER beginning = {};
        if (FAILED(stream->Seek(beginning, STREAM_SEEK_SET, nullptr))) return false;
        std::vector<BYTE> encoded(static_cast<std::size_t>(stat.cbSize.QuadPart));
        ULONG read = 0;
        if (FAILED(stream->Read(encoded.data(), static_cast<ULONG>(encoded.size()), &read)) ||
            read != encoded.size()) return false;
        bytes.swap(encoded);
        return true;
    } catch (const std::bad_alloc&) { return false; }
}

bool GdiImage::Encode(BYTE*& bytes, long& size, DWORD format) const {
    bytes = nullptr; size = 0;
    std::vector<BYTE> encoded;
    if (!Encode(encoded, format)) return false;
    bytes = static_cast<BYTE*>(std::malloc(encoded.size()));
    if (!bytes) return false;
    std::memcpy(bytes, encoded.data(), encoded.size());
    size = static_cast<long>(encoded.size());
    return true;
}
void GdiImage::FreeMemory(void* bytes) { std::free(bytes); }

bool GdiImage::Save(const wchar_t* filename, DWORD format) const {
    if (!filename || !*filename || !IsValid() || !Ready()) return false;
    try {
        CLSID clsid;
        if (!Encoder(format, clsid)) return false;
        Gdiplus::Bitmap bitmap(width_, height_, static_cast<INT>(stride_), PixelFormat24bppRGB,
                               const_cast<BYTE*>(GetBits()));
        if (bitmap.GetLastStatus() != Gdiplus::Ok) return false;
        ULONG quality = quality_;
        auto params = QualityParameters(quality);
        return bitmap.Save(filename, &clsid, format == GDI_IMAGE_JPG ? &params : nullptr) == Gdiplus::Ok;
    } catch (const std::bad_alloc&) { return false; }
}

bool GdiImage::MixFrom(const GdiImage& image, int x, int y) {
    if (!IsValid() || !image.IsValid() || x < 0 || y < 0 ||
        image.width_ > width_ || image.height_ > height_ ||
        x > width_ - image.width_ || y > height_ - image.height_) return false;
    if (this == &image) return x == 0 && y == 0;
    for (int row = 0; row < image.height_; ++row)
        std::memcpy(GetBits() + (y + row) * stride_ + x * 3,
                    image.GetBits() + row * image.stride_, static_cast<std::size_t>(image.width_) * 3);
    return true;
}

bool GdiImage::Stretch(HDC dc, int x, int y, int width, int height) const {
    if (!dc || !IsValid() || width <= 0 || height <= 0) return false;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width_;
    info.bmiHeader.biHeight = -height_;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;
    const int oldMode = SetStretchBltMode(dc, HALFTONE);
    POINT oldOrigin = {};
    SetBrushOrgEx(dc, 0, 0, &oldOrigin);
    const int result = StretchDIBits(dc, x, y, width, height, 0, 0, width_, height_,
                                    GetBits(), &info, DIB_RGB_COLORS, SRCCOPY);
    SetBrushOrgEx(dc, oldOrigin.x, oldOrigin.y, nullptr);
    SetStretchBltMode(dc, oldMode);
    return result != GDI_ERROR && result != 0;
}
void GdiImage::SetJpegQuality(int quality) { quality_ = (std::max)(0, (std::min)(100, quality)); }
