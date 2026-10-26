#include "ScreenDisplay.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <memory>

namespace {
// Common window DCs belong to the current paint/layout state. Do not retain one
// across resizing, scrolling or destruction of the MFC child control.
struct WindowDC {
    HWND window;
    HDC handle;
    explicit WindowDC(HWND hwnd) : window(hwnd), handle(IsWindow(hwnd) ? GetDC(hwnd) : nullptr) {}
    ~WindowDC() { if (handle) ReleaseDC(window, handle); }
};
}

bool ScreenViewGeometry::SetSourceSize(int width, int height) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return false;
    sourceWidth_ = width; sourceHeight_ = height;
    Recalculate();
    return true;
}
bool ScreenViewGeometry::SetViewportSize(int width, int height) {
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768) return false;
    viewportWidth_ = width; viewportHeight_ = height;
    Recalculate();
    return true;
}
void ScreenViewGeometry::Fit() { zoom_ = 1.0; Recalculate(); }
bool ScreenViewGeometry::Zoom(double factor) {
    if (!std::isfinite(factor) || factor <= 0 || sourceWidth_ <= 0 || viewportWidth_ <= 0)
        return false;
    const double next = (std::max)(0.1, (std::min)(8.0, zoom_ * factor));
    if (next == zoom_) return false;
    zoom_ = next;
    Recalculate();
    return true;
}
void ScreenViewGeometry::Recalculate() {
    displayRect_ = RECT{};
    if (sourceWidth_ <= 0 || sourceHeight_ <= 0 || viewportWidth_ <= 0 || viewportHeight_ <= 0)
        return;
    const double fit = (std::min)(static_cast<double>(viewportWidth_) / sourceWidth_,
                                  static_cast<double>(viewportHeight_) / sourceHeight_);
    const double scale = fit * zoom_;
    // Always derive both dimensions from the original source, never from a
    // previous rounded display rectangle. Even a subpixel axis needs one pixel.
    const int width = (std::max)(1, static_cast<int>(std::floor(sourceWidth_ * scale + 0.5)));
    const int height = (std::max)(1, static_cast<int>(std::floor(sourceHeight_ * scale + 0.5)));
    displayRect_.left = (viewportWidth_ - width) / 2;
    displayRect_.top = (viewportHeight_ - height) / 2;
    displayRect_.right = displayRect_.left + width;
    displayRect_.bottom = displayRect_.top + height;
}
bool ScreenViewGeometry::MapWindowPointToServer(int x, int y, int& serverX, int& serverY) const {
    if (x < 0 || y < 0 || x >= viewportWidth_ || y >= viewportHeight_ ||
        x < displayRect_.left || x >= displayRect_.right ||
        y < displayRect_.top || y >= displayRect_.bottom) return false;
    const int width = displayRect_.right - displayRect_.left;
    const int height = displayRect_.bottom - displayRect_.top;
    if (width <= 0 || height <= 0 || sourceWidth_ <= 0 || sourceHeight_ <= 0) return false;
    // Map the displayed end pixels to the source end pixels, so desktop edges
    // remain reachable when downscaled. A one-pixel axis represents its centre.
    serverX = width == 1 ? (sourceWidth_ - 1) / 2 : static_cast<int>(
        static_cast<std::int64_t>(x - displayRect_.left) * (sourceWidth_ - 1) / (width - 1));
    serverY = height == 1 ? (sourceHeight_ - 1) / 2 : static_cast<int>(
        static_cast<std::int64_t>(y - displayRect_.top) * (sourceHeight_ - 1) / (height - 1));
    return true;
}

ScreenDisplay::~ScreenDisplay() { ReleaseDevice(); }
void ScreenDisplay::ReleaseDevice() {
    if (memoryDC_ && originalBitmap_) SelectObject(memoryDC_, originalBitmap_);
    if (bitmap_) DeleteObject(bitmap_);
    if (memoryDC_) DeleteDC(memoryDC_);
    window_ = nullptr; memoryDC_ = nullptr;
    bitmap_ = nullptr; originalBitmap_ = nullptr;
}
bool ScreenDisplay::Initialize(HWND window, int windowWidth, int windowHeight,
                                int serverWidth, int serverHeight) {
    if (!IsWindow(window) || windowWidth <= 0 || windowHeight <= 0) return false;
    GdiImage frame;
    if (!frame.Create(serverWidth, serverHeight)) return false;
    ReleaseDevice();
    window_ = window;
    WindowDC windowDC(window_);
    memoryDC_ = windowDC.handle ? CreateCompatibleDC(windowDC.handle) : nullptr;
    if (!memoryDC_) { ReleaseDevice(); return false; }
    frame_ = std::move(frame);
    serverWidth_ = serverWidth; serverHeight_ = serverHeight;
    view_ = ScreenViewGeometry{};
    view_.SetSourceSize(serverWidth, serverHeight);
    RECT rect = {};
    if (!GetClientRect(window_, &rect)) { ReleaseDevice(); return false; }
    if (!SetSize((std::max)(1L, rect.right), (std::max)(1L, rect.bottom))) {
        ReleaseDevice(); return false;
    }
    return true;
}
bool ScreenDisplay::SetSize(int width, int height) {
    if (!IsWindow(window_) || !memoryDC_ || width <= 0 || height <= 0 || width > 32768 || height > 32768)
        return false;
    if (bitmap_ && width == windowWidth_ && height == windowHeight_) {
        view_.SetViewportSize(width, height);
        return true;
    }
    WindowDC windowDC(window_);
    if (!windowDC.handle) return false;
    HBITMAP replacement = CreateCompatibleBitmap(windowDC.handle, width, height);
    if (!replacement) return false;
    HGDIOBJ old = SelectObject(memoryDC_, replacement);
    if (!old || old == HGDI_ERROR) { DeleteObject(replacement); return false; }
    if (!originalBitmap_) originalBitmap_ = old;
    if (bitmap_) DeleteObject(bitmap_);
    bitmap_ = replacement; windowWidth_ = width; windowHeight_ = height;
    view_.SetViewportSize(width, height);
    return ReflashDC();
}
void ScreenDisplay::Display() {
    if (!memoryDC_ || !bitmap_) return;
    WindowDC windowDC(window_);
    if (!windowDC.handle) return;
    PatBlt(memoryDC_, 0, 0, windowWidth_, windowHeight_, BLACKNESS);
    const RECT rect = view_.GetDisplayRect();
    frame_.Stretch(memoryDC_, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    BitBlt(windowDC.handle, 0, 0, windowWidth_, windowHeight_, memoryDC_, 0, 0, SRCCOPY);
}
void ScreenDisplay::RecvImageMsg(ByteArrayMsg* message) {
    std::unique_ptr<ByteArrayMsg> owner(message);
    if (!message || message->GetType() != RCS_MSG_IMAGE || message->GetSize() < 21) return;
    // Queue objects can be ByteArrayMsg. Validate the header before decoding/copying data.
    const char* bytes = message->GetBufferPtr();
    if (!bytes) return;
    RECT rect = {};
    std::uint32_t encodedSize = 0;
    std::memcpy(&rect, bytes + 1, 16);
    std::memcpy(&encodedSize, bytes + 17, 4);
    if (rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top ||
        rect.right > serverWidth_ || rect.bottom > serverHeight_ ||
        encodedSize != message->GetSize() - 21 || encodedSize > 16u * 1024u * 1024u) return;
    GdiImage decoded;
    if (!decoded.Decode(reinterpret_cast<const BYTE*>(bytes + 21), encodedSize)) return;
    if (decoded.GetWidth() != rect.right - rect.left || decoded.GetHeight() != rect.bottom - rect.top)
        return;
    frame_.MixFrom(decoded, rect.left, rect.top);
}
void ScreenDisplay::SetServerSize(int width, int height) {
    if (width == serverWidth_ && height == serverHeight_) return;
    GdiImage replacement;
    if (!replacement.Create(width, height)) return;
    frame_ = std::move(replacement); serverWidth_ = width; serverHeight_ = height;
    view_.SetSourceSize(width, height);
}
bool ScreenDisplay::ScaleUp() {
    const bool changed = view_.Zoom(1.25);
    ReflashDC(); return changed;
}
bool ScreenDisplay::ScaleDown() {
    const bool changed = view_.Zoom(0.8);
    ReflashDC(); return changed;
}
bool ScreenDisplay::ReflashDC() {
    return memoryDC_ && bitmap_ && PatBlt(memoryDC_, 0, 0, windowWidth_, windowHeight_, BLACKNESS) != FALSE;
}
void ScreenDisplay::Scale2LocalSize() {
    view_.Fit(); ReflashDC();
}
bool ScreenDisplay::MapWindowPointToServer(int x, int y, int& serverX, int& serverY) const {
    return view_.MapWindowPointToServer(x, y, serverX, serverY);
}
