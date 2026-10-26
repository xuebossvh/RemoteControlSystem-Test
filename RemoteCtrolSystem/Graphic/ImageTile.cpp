#include "ImageTile.h"
#include <algorithm>
#include <cstring>
#include <new>

bool ImageTile::Copy(const BYTE* pixels, int width, int height, std::size_t stride,
                     RECT rect, bool topDown) {
    if (!pixels || width <= 0 || height <= 0 || rect.left < 0 || rect.top < 0 ||
        rect.right <= rect.left || rect.bottom <= rect.top ||
        rect.right > width || rect.bottom > height ||
        stride < static_cast<std::size_t>(width) * 3) return false;
    const std::size_t rowSize = static_cast<std::size_t>(rect.right - rect.left) * 3;
    const int tileHeight = rect.bottom - rect.top;
    if (rowSize > 256u * 1024u * 1024u / static_cast<std::size_t>(tileHeight)) return false;
    try {
        std::vector<BYTE> result(rowSize * tileHeight);
        for (int y = 0; y < tileHeight; ++y) {
            const int sourceY = topDown ? rect.top + y : height - 1 - rect.top - y;
            std::memcpy(result.data() + y * rowSize, pixels + sourceY * stride + rect.left * 3, rowSize);
        }
        pixels_.swap(result); rect_ = rect;
        return true;
    } catch (const std::bad_alloc&) { return false; }
}

bool ImageTile::CopyFrom(const GdiImage& image, RECT rect) {
    return Copy(image.GetBits(), image.GetWidth(), image.GetHeight(), image.GetStride(), rect, true);
}
bool ImageTile::SetImageTile(char* pixels, RECT screen, RECT tile) {
    return Copy(reinterpret_cast<const BYTE*>(pixels), screen.right - screen.left,
                screen.bottom - screen.top, static_cast<std::size_t>(screen.right - screen.left) * 3, tile, true);
}
bool ImageTile::SetImageTileReverse(char* pixels, RECT screen, RECT tile) {
    return Copy(reinterpret_cast<const BYTE*>(pixels), screen.right - screen.left,
                screen.bottom - screen.top, static_cast<std::size_t>(screen.right - screen.left) * 3, tile, false);
}
bool ImageTile::ImageBits(char* destination) const {
    if (!destination || pixels_.empty()) return false;
    std::memcpy(destination, pixels_.data(), pixels_.size()); return true;
}
bool ImageTile::Matches(const ImageTile& other) const {
    return rect_.left == other.rect_.left && rect_.top == other.rect_.top &&
           rect_.right == other.rect_.right && rect_.bottom == other.rect_.bottom && pixels_ == other.pixels_;
}
void SwapImageTile(ImageTile& first, ImageTile& second) {
    std::swap(first.rect_, second.rect_); first.pixels_.swap(second.pixels_);
}
