#include "../GdiImage.h"
#include "../ImageTile.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
void Check(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
void Fill(GdiImage& image) {
    // Four different corners expose vertical flips and B/R channel swaps.
    const BYTE colors[4][3] = {{0, 0, 255}, {0, 255, 0}, {255, 0, 0}, {255, 255, 255}};
    for (int y = 0; y < image.GetHeight(); ++y)
        for (int x = 0; x < image.GetWidth(); ++x) {
            const int index = (x >= image.GetWidth() / 2) + 2 * (y >= image.GetHeight() / 2);
            std::memcpy(image.GetBits() + y * image.GetStride() + x * 3, colors[index], 3);
        }
}
void SamePixels(const GdiImage& a, const GdiImage& b) {
    Check(a.GetWidth() == b.GetWidth() && a.GetHeight() == b.GetHeight(), "image size");
    for (int y = 0; y < a.GetHeight(); ++y)
        Check(std::memcmp(a.GetBits() + y * a.GetStride(), b.GetBits() + y * b.GetStride(),
                          static_cast<std::size_t>(a.GetWidth()) * 3) == 0, "lossless pixels/stride");
}
void SameSample(const GdiImage& a, const GdiImage& b, int x, int y, int tolerance) {
    const BYTE* left = a.GetBits() + y * a.GetStride() + x * 3;
    const BYTE* right = b.GetBits() + y * b.GetStride() + x * 3;
    for (int c = 0; c < 3; ++c)
        Check(std::abs(static_cast<int>(left[c]) - right[c]) <= tolerance, "JPEG corner/channel orientation");
}
}

int main() {
    GdiImage original;
    Check(original.Create(37, 29), "create odd width");
    Check(original.GetStride() == 112, "four-byte stride");
    Fill(original);

    GdiImage copied = original;
    copied.GetBits()[0] = 23;
    Check(original.GetBits()[0] == 0, "deep copy");

    std::vector<BYTE> bottomUp(original.GetStride() * original.GetHeight());
    for (int y = 0; y < original.GetHeight(); ++y)
        std::memcpy(bottomUp.data() + (original.GetHeight() - y - 1) * original.GetStride(),
                    original.GetBits() + y * original.GetStride(), original.GetStride());
    GdiImage imported;
    Check(imported.CreateFromArray(bottomUp.data(), 37, 29, 24, 112, false), "bottom-up import");
    SamePixels(original, imported);

    std::vector<BYTE> bytes;
    GdiImage decoded;
    Check(original.Encode(bytes, GDI_IMAGE_PNG), "PNG encode");
    Check(decoded.Decode(bytes.data(), bytes.size(), GDI_IMAGE_PNG), "PNG decode");
    SamePixels(original, decoded);

    original.SetJpegQuality(95);
    Check(original.Encode(bytes), "JPEG encode");
    Check(decoded.Decode(bytes.data(), bytes.size()), "JPEG decode");
    Check(decoded.GetWidth() == 37 && decoded.GetHeight() == 29, "JPEG odd dimensions");
    SameSample(original, decoded, 5, 5, 20);
    SameSample(original, decoded, 30, 5, 20);
    SameSample(original, decoded, 5, 23, 20);
    SameSample(original, decoded, 30, 23, 20);

    // Unequal rows/columns cover last-tile edges and top-left placement.
    GdiImage composite;
    Check(composite.Create(37, 29), "composite canvas");
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x) {
        RECT rect = {x * 37 / 3, y * 29 / 2, (x + 1) * 37 / 3, (y + 1) * 29 / 2};
        ImageTile tile;
        Check(tile.CopyFrom(original, rect), "tile crop");
        GdiImage fragment;
        const int width = rect.right - rect.left, height = rect.bottom - rect.top;
        Check(fragment.CreateFromArray(reinterpret_cast<const BYTE*>(tile.GetBitsPtr()), width, height,
                                        24, width * 3, true), "packed tile import");
        Check(composite.MixFrom(fragment, rect.left, rect.top), "tile placement");
        ImageTile duplicate;
        Check(duplicate.CopyFrom(original, rect) && tile.Matches(duplicate), "unchanged tile");
        duplicate.GetBitsPtr()[0] ^= 1;
        Check(!tile.Matches(duplicate), "one-pixel tile change");
        fragment.SetJpegQuality(95);
        Check(fragment.Encode(bytes), "tile JPEG encode");
        GdiImage tileDecoded;
        Check(tileDecoded.Decode(bytes.data(), bytes.size()), "tile JPEG decode");
        Check(tileDecoded.GetWidth() == width && tileDecoded.GetHeight() == height, "tile JPEG dimensions");
    }
    SamePixels(original, composite);

    // Test 32-bit device bitmaps without reading the user's desktop.
    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = 37;
    bitmapInfo.bmiHeader.biHeight = -29;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    Check(bitmap && bits, "32-bit synthetic DIB");
    for (int y = 0; y < 29; ++y) for (int x = 0; x < 37; ++x) {
        BYTE* pixel = static_cast<BYTE*>(bits) + (y * 37 + x) * 4;
        std::memcpy(pixel, original.GetBits() + y * original.GetStride() + x * 3, 3);
        pixel[3] = 255;
    }
    GdiImage converted;
    Check(converted.CreateFromHBITMAP(bitmap), "32-to-24-bit conversion");
    SamePixels(original, converted);
    HDC dc = CreateCompatibleDC(nullptr);
    Check(dc != nullptr, "offscreen DC");
    HGDIOBJ old = SelectObject(dc, bitmap);
    Check(old && old != HGDI_ERROR, "select synthetic DIB");
    Check(original.Stretch(dc, 0, 0, 37, 29), "offscreen StretchDIBits");
    SelectObject(dc, old); DeleteDC(dc);
    Check(converted.CreateFromHBITMAP(bitmap), "read offscreen render");
    SamePixels(original, converted);
    DeleteObject(bitmap);

    const BYTE invalid[] = {1, 2, 3, 4};
    Check(!decoded.Decode(invalid, sizeof(invalid)), "invalid JPEG rejected");
    const BYTE truncated[] = {0xff, 0xd8, 0xff, 0xe0};
    Check(!decoded.Decode(truncated, sizeof(truncated)), "truncated JPEG rejected");
    Check(!original.Create(0, 10) && !original.Create(16384, 16384), "invalid/oversized allocation rejected");
    Check(!composite.MixFrom(original, 1, 0), "out-of-bounds composition rejected");
    Check(!original.Encode(bytes, 999) && bytes.empty(), "unsupported codec rejected");
    RECT invalidRect = {0, 0, 38, 29};
    ImageTile tile;
    Check(!tile.CopyFrom(original, invalidRect), "out-of-bounds crop rejected");
    std::cout << "PASS: odd-width BGR, orientation, deep copy, PNG/JPEG, tiles, 32-bit DIB, rendering, invalid inputs\n";
    return 0;
}
