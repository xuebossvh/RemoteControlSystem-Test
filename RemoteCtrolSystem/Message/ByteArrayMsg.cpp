#include "ByteArrayMsg.h"
#include <cstring>

ByteArrayMsg::ByteArrayMsg() : m_buffer(NULL), m_size(0) {}
ByteArrayMsg::~ByteArrayMsg() { delete[] m_buffer; }
unsigned __int8 ByteArrayMsg::GetType() { return m_size ? static_cast<unsigned char>(m_buffer[0]) : 0; }
size_t ByteArrayMsg::GetBuffer(void* buffer, size_t len) {
    if (!buffer || len < m_size) return 0;
    if (m_size) memcpy(buffer, m_buffer, m_size);
    return m_size;
}
size_t ByteArrayMsg::SetBuffer(const void* buffer) {
    if (!buffer) return 0;
    // Compatibility overload for callers already holding a complete message.
    const size_t length = GetSize(buffer, 21);
    if (!length || length == INVALID_SIZE) return 0;
    return SetBuffer(buffer, length);
}
size_t ByteArrayMsg::SetBuffer(const void* buffer, size_t available) {
    if (!buffer || available > MAX_IMAGE_BYTES + 21) return 0;
    const size_t length = GetSize(buffer, static_cast<int>(available));
    if (!length || length == INVALID_SIZE || length > available) return 0;
    char* copy = new char[length];
    memcpy(copy, buffer, length);
    delete[] m_buffer;
    m_buffer = copy;
    m_size = length;
    return length;
}
char* ByteArrayMsg::GetBufferPtr() { return m_buffer; }
size_t ByteArrayMsg::GetSize() { return m_size; }
size_t ByteArrayMsg::GetSize(const void* buffer, int available) {
    if (!buffer || available <= 0) return 0;
    const unsigned char* bytes = static_cast<const unsigned char*>(buffer);
    switch (bytes[0]) {
    case RCS_MSG_IMAGE: {
        if (available < 21) return 0;
        uint32_t length = 0;
        memcpy(&length, bytes + 17, sizeof(length));
        if (!length || length > MAX_IMAGE_BYTES) return INVALID_SIZE;
        return 21u + length;
    }
    case RCS_MSG_MOUSE: return 13;
    case RCS_MSG_KEYBOARD: return 9;
    case RCS_MSG_USER: return 41;
    case RCS_MSG_SETUP: return 13;
    default: return INVALID_SIZE;
    }
}
