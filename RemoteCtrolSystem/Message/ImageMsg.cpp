#include "ImageMsg.h"
#include <cstring>
#include <cstdint>
ImageMsg::ImageMsg() {}
ImageMsg::~ImageMsg() {}
size_t ImageMsg::GetImageSize() {
    if (m_size < 21 || GetType() != RCS_MSG_IMAGE) return 0;
    uint32_t length = 0; memcpy(&length, m_buffer + 17, 4);
    return length == m_size - 21 ? length : 0;
}
RECT ImageMsg::GetImageRect() {
    RECT rect = {};
    if (m_size >= 21 && GetType() == RCS_MSG_IMAGE) memcpy(&rect, m_buffer + 1, 16);
    return rect;
}
bool ImageMsg::GetImageBuffer(char* buffer) {
    if (!buffer || !GetImageSize()) return false;
    memcpy(buffer, m_buffer + 21, GetImageSize()); return true;
}
char* ImageMsg::GetImageBufferPtr() { return GetImageSize() ? m_buffer + 21 : NULL; }
bool ImageMsg::PacketMsg(RECT rect, size_t imageSize, char* buffer) {
    if (!buffer || !imageSize || imageSize > MAX_IMAGE_BYTES || rect.right <= rect.left || rect.bottom <= rect.top) return false;
    const size_t length = 21 + imageSize;
    char* data = new char[length];
    data[0] = RCS_MSG_IMAGE;
    memcpy(data + 1, &rect, 16);
    const uint32_t size = static_cast<uint32_t>(imageSize);
    memcpy(data + 17, &size, 4);
    memcpy(data + 21, buffer, imageSize);
    delete[] m_buffer; m_buffer = data; m_size = length;
    return true;
}
