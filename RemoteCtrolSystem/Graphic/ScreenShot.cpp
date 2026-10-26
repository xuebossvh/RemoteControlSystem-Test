#include "ScreenShot.h"
#include <algorithm>
#include <memory>
#include <new>

namespace {
struct DesktopDC {
    HDC handle = GetDC(nullptr);
    ~DesktopDC() { if (handle) ReleaseDC(nullptr, handle); }
};
struct MemoryDC {
    HDC handle;
    explicit MemoryDC(HDC screen) : handle(CreateCompatibleDC(screen)) {}
    ~MemoryDC() { if (handle) DeleteDC(handle); }
};
struct CapturedBitmap {
    HBITMAP handle;
    CapturedBitmap(HDC dc, int width, int height) : handle(CreateCompatibleBitmap(dc, width, height)) {}
    ~CapturedBitmap() { if (handle) DeleteObject(handle); }
};
struct BitmapSelection {
    HDC dc;
    HGDIOBJ previous;
    BitmapSelection(HDC dc, HBITMAP bitmap) : dc(dc), previous(SelectObject(dc, bitmap)) {}
    ~BitmapSelection() { Reset(); }
    bool IsValid() const { return previous && previous != HGDI_ERROR; }
    void Reset() { if (IsValid()) SelectObject(dc, previous); previous = nullptr; }
};

bool CaptureDesktop(GdiImage& image) {
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    if (width <= 0 || height <= 0) return false;
    DesktopDC screen;
    if (!screen.handle) return false;
    MemoryDC memory(screen.handle);
    if (!memory.handle) return false;
    CapturedBitmap bitmap(screen.handle, width, height);
    if (!bitmap.handle) return false;
    BitmapSelection selected(memory.handle, bitmap.handle);
    if (!selected.IsValid() || !BitBlt(memory.handle, 0, 0, width, height, screen.handle, 0, 0, SRCCOPY))
        return false;
    // GetDIBits requires the bitmap to be deselected from the memory DC.
    selected.Reset();
    return image.CreateFromHBITMAP(bitmap.handle);
}
}

bool ScreenShot::Init(int horizontal, int vertical) {
    if (horizontal < 1 || horizontal > 64 || vertical < 1 || vertical > 64) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    columns_ = horizontal; rows_ = vertical;
    previous_.clear(); // A grid change requires a complete refresh.
    fullRefreshPending_ = true;
    return true;
}
bool ScreenShot::SetTileNumber(int horizontal, int vertical) { return Init(horizontal, vertical); }
bool ScreenShot::SetQuality(int quality) {
    if (quality < 0 || quality > 100) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (quality_ != quality) { quality_ = quality; previous_.clear(); fullRefreshPending_ = true; }
    return true;
}

bool ScreenShot::ShotScreenImage(std::queue<ByteArrayMsg*>* messages) {
    if (!messages) return false;
    GdiImage screen;
    if (!CaptureDesktop(screen)) { RequestFullRefresh(); return false; }
    return EncodeFrame(screen, messages, GetTickCount64());
}
void ScreenShot::RequestFullRefresh() {
    std::lock_guard<std::mutex> lock(mutex_);
    fullRefreshPending_ = true;
}
bool ScreenShot::EncodeFrame(const GdiImage& screen, std::queue<ByteArrayMsg*>* messages, ULONGLONG nowMilliseconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    pixelChanged_ = false;
    if (!messages || !screen.IsValid()) { fullRefreshPending_ = true; return false; }
    const int width = screen.GetWidth(), height = screen.GetHeight();
    const int columns = (std::min)(columns_, width), rows = (std::min)(rows_, height);
    try {
        if (width != width_ || height != height_) {
            previous_.clear(); width_ = width; height_ = height; sizeChangePending_ = true;
            fullRefreshPending_ = true;
        }
        if (previous_.size() != static_cast<std::size_t>(columns * rows))
            previous_.resize(columns * rows);
        // The wire format has no frame acknowledgement. Periodic full updates
        // recover an unchanged tile discarded after a decode or resize failure.
        // Black pixels are legitimate content and never used as a failure signal.
        const bool fullRefresh = fullRefreshPending_ || !haveFullRefresh_ ||
            nowMilliseconds < lastFullRefresh_ || nowMilliseconds - lastFullRefresh_ >= 5000;
        bool succeeded = true;
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < columns; ++x) {
                // Keep wire coordinates in the conventional top-left screen origin.
                RECT rect = { x * width / columns, y * height / rows,
                              (x + 1) * width / columns, (y + 1) * height / rows };
                ImageTile current;
                if (!current.CopyFrom(screen, rect)) { succeeded = false; continue; }
                ImageTile& previous = previous_[y * columns + x];
                if (!fullRefresh && current.Matches(previous)) continue;
                GdiImage encoder;
                const int tileWidth = rect.right - rect.left;
                const int tileHeight = rect.bottom - rect.top;
                if (!encoder.CreateFromArray(reinterpret_cast<const BYTE*>(current.GetBitsPtr()),
                        tileWidth, tileHeight, 24, static_cast<std::size_t>(tileWidth) * 3, true)) {
                    succeeded = false; continue;
                }
                encoder.SetJpegQuality(quality_);
                std::vector<BYTE> encoded;
                if (!encoder.Encode(encoded) || encoded.size() > 16u * 1024u * 1024u) {
                    succeeded = false; continue;
                }
                std::unique_ptr<ImageMsg> message(new ImageMsg);
                if (!message->PacketMsg(rect, encoded.size(), reinterpret_cast<char*>(encoded.data()))) {
                    succeeded = false; continue;
                }
                messages->push(message.get());
                message.release();
                SwapImageTile(current, previous); // Retry failed tiles next time.
            }
        }
        if (succeeded && fullRefresh) {
            lastFullRefresh_ = nowMilliseconds;
            haveFullRefresh_ = true;
            fullRefreshPending_ = false;
        } else if (!succeeded) {
            fullRefreshPending_ = true;
        }
        if (succeeded) {
            // Announce a new size with the first successfully encoded update,
            // even when the first capture at that size failed partway through.
            pixelChanged_ = sizeChangePending_;
            sizeChangePending_ = false;
        }
        return succeeded;
    } catch (const std::bad_alloc&) { fullRefreshPending_ = true; return false; }
}
int ScreenShot::GetScreenWidth() { std::lock_guard<std::mutex> lock(mutex_); return width_; }
int ScreenShot::GetScreenHeight() { std::lock_guard<std::mutex> lock(mutex_); return height_; }
bool ScreenShot::IsPixelChange() { std::lock_guard<std::mutex> lock(mutex_); return pixelChanged_; }
