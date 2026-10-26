#include "../ScreenDisplay.h"
#include "../ScreenShot.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <queue>

namespace {
void Check(bool value, const char* description) {
    if (!value) { std::cerr << "FAIL: " << description << '\n'; std::exit(1); }
}
void FillCorners(GdiImage& image) {
    const BYTE colors[4][3] = {{0, 0, 255}, {0, 255, 0}, {255, 0, 0}, {255, 255, 255}};
    for (int y = 0; y < image.GetHeight(); ++y) for (int x = 0; x < image.GetWidth(); ++x) {
        const int color = (x >= image.GetWidth() / 2) + 2 * (y >= image.GetHeight() / 2);
        std::memcpy(image.GetBits() + y * image.GetStride() + x * 3, colors[color], 3);
    }
}
void CheckPixel(const BYTE* pixel, int b, int g, int r, int tolerance = 25) {
    Check(std::abs(pixel[0] - b) <= tolerance && std::abs(pixel[1] - g) <= tolerance &&
          std::abs(pixel[2] - r) <= tolerance, "expected BGR colour");
}
void Deliver(std::queue<ByteArrayMsg*>& messages, ScreenDisplay& display, int drop = -1) {
    int index = 0;
    while (!messages.empty()) {
        ByteArrayMsg* message = messages.front(); messages.pop();
        if (index++ == drop) delete message;
        else Check(display.RecvImageMsg(message), "decode and compose valid tile");
    }
}
struct OffscreenTarget {
    static const int width = 83, height = 61;
    HWND window = nullptr;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    BYTE* pixels = nullptr;
    OffscreenTarget() {
        // Never show a window or read the user's desktop. Render to our own DIB.
        window = CreateWindowExW(0, L"STATIC", L"RCS offscreen recovery test", WS_POPUP,
                                 0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "hidden render window");
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        dc = CreateCompatibleDC(nullptr);
        Check(bitmap && dc && bits, "offscreen bitmap and DC");
        pixels = static_cast<BYTE*>(bits);
        old = SelectObject(dc, bitmap);
        Check(old && old != HGDI_ERROR, "select offscreen target");
    }
    ~OffscreenTarget() {
        if (dc && old) SelectObject(dc, old);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        if (window) DestroyWindow(window);
    }
    void Clear() { std::memset(pixels, 0, width * height * 4); }
    const BYTE* Pixel(int x, int y) const { return pixels + (y * width + x) * 4; }
};
void CheckRenderedCorners(ScreenDisplay& display, OffscreenTarget& target) {
    target.Clear();
    Check(display.Render(target.dc), "render persistent frame");
    GdiFlush();
    const RECT rect = display.GetDisplayRect();
    const int left = rect.left + (rect.right - rect.left) / 4;
    const int right = rect.left + 3 * (rect.right - rect.left) / 4;
    const int top = rect.top + (rect.bottom - rect.top) / 4;
    const int bottom = rect.top + 3 * (rect.bottom - rect.top) / 4;
    CheckPixel(target.Pixel(left, top), 0, 0, 255);
    CheckPixel(target.Pixel(right, top), 0, 255, 0);
    CheckPixel(target.Pixel(left, bottom), 255, 0, 0);
    CheckPixel(target.Pixel(right, bottom), 255, 255, 255);
    CheckPixel(target.Pixel(0, 0), 0, 0, 0, 0); // Letterbox/pillarbox is always black.
}
}

int main() {
    OffscreenTarget target;
    ScreenDisplay display;
    Check(display.Initialize(target.window, target.width, target.height, 64, 36), "display initialization");
    ScreenShot encoder;
    Check(encoder.Init(2, 2) && encoder.SetQuality(95), "encoder initialization");
    GdiImage frame;
    Check(frame.Create(64, 36), "synthetic frame allocation");
    FillCorners(frame);
    std::queue<ByteArrayMsg*> messages;
    Check(encoder.EncodeFrame(frame, &messages, 100) && messages.size() == 4, "initial complete desktop");
    Deliver(messages, display);
    CheckRenderedCorners(display, target);

    // Repainting an erased target must work with no newly received network data.
    Check(encoder.EncodeFrame(frame, &messages, 200) && messages.empty(), "unchanged desktop is incremental");
    CheckRenderedCorners(display, target);
    Check(!display.SetSize(0, 0), "minimized zero size does not destroy the frame");
    CheckRenderedCorners(display, target);
    Check(display.SetSize(19, 47), "resize to portrait viewport");
    CheckRenderedCorners(display, target);
    Check(display.SetSize(target.width, target.height), "restore original viewport");
    CheckRenderedCorners(display, target);
    Check(!display.Render(nullptr), "bad paint DC rejected");
    CheckRenderedCorners(display, target);

    const DWORD resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 30; ++i) {
        Check(display.SetSize(19, 47) && display.SetSize(target.width, target.height), "repeated restore");
        CheckRenderedCorners(display, target);
    }
    Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= resources + 1, "resize releases old GDI bitmaps");

    std::unique_ptr<ImageMsg> invalid(new ImageMsg);
    char garbage[4] = {1, 2, 3, 4};
    RECT full = {0, 0, 64, 36};
    Check(invalid->PacketMsg(full, sizeof(garbage), garbage), "corrupt JPEG packet envelope");
    Check(!display.RecvImageMsg(invalid.release()), "bad JPEG cannot overwrite the persistent image");
    CheckRenderedCorners(display, target);

    // Recreate the receiver, deliberately drop one valid tile, then keep the
    // source static. Periodic refresh must recover it without a pixel change.
    Check(display.Initialize(target.window, target.width, target.height, 64, 36), "receiver recovery reset");
    encoder.RequestFullRefresh();
    Check(encoder.EncodeFrame(frame, &messages, 300) && messages.size() == 4, "requested full refresh");
    Deliver(messages, display, 0);
    GdiImage partial = display.GetDisplayImage();
    CheckPixel(partial.GetBits() + 5 * partial.GetStride() + 5 * 3, 0, 0, 0, 0);
    Check(encoder.EncodeFrame(frame, &messages, 5299) && messages.empty(), "refresh interval not yet elapsed");
    Check(encoder.EncodeFrame(frame, &messages, 5300) && messages.size() == 4, "five-second complete recovery frame");
    Deliver(messages, display);
    CheckRenderedCorners(display, target);

    GdiImage unavailable;
    Check(!encoder.EncodeFrame(unavailable, &messages, 5400), "unavailable capture rejected");
    Check(encoder.EncodeFrame(frame, &messages, 5401) && messages.size() == 4, "valid capture after failure is complete");
    Deliver(messages, display);
    CheckRenderedCorners(display, target);

    Check(frame.Create(35, 61), "new source resolution");
    FillCorners(frame);
    Check(encoder.EncodeFrame(frame, &messages, 5500) && messages.size() == 4 && encoder.IsPixelChange(),
          "resolution change resets tile history");
    display.SetServerSize(35, 61);
    Deliver(messages, display);
    CheckRenderedCorners(display, target);

    Check(frame.Create(35, 61), "legitimate black desktop");
    Check(encoder.EncodeFrame(frame, &messages, 5600) && messages.size() == 4, "black pixels are valid content");
    Deliver(messages, display);
    Check(display.Render(target.dc), "render legitimate black desktop");
    GdiFlush();
    CheckPixel(target.Pixel(41, 30), 0, 0, 0, 0);
    std::cout << "PASS: repaint without network, minimize/restore geometry, GDI lifetime, bad JPEG preservation, periodic tile recovery\n";
}
