#ifndef RCS_MESSAGE_FRAMER_H
#define RCS_MESSAGE_FRAMER_H
#include "ByteArrayMsg.h"
#include <vector>
#include <algorithm>

// Owned by one connection/receiving thread. Retains partial headers and payloads.
class MessageFramer {
public:
    bool Append(const char* data, size_t length) {
        if (!data || length > ByteArrayMsg::MAX_IMAGE_BYTES + 65557u ||
            m_bytes.size() + length > ByteArrayMsg::MAX_IMAGE_BYTES + 65557u) return false;
        m_bytes.insert(m_bytes.end(), data, data + length);
        return true;
    }
    // 1 = complete message, 0 = needs more bytes, -1 = malformed frame.
    int Pop(ByteArrayMsg& message) {
        if (m_bytes.empty()) return 0;
        const size_t length = ByteArrayMsg::GetSize(m_bytes.data(), static_cast<int>(m_bytes.size()));
        if (length == ByteArrayMsg::INVALID_SIZE) return -1;
        if (!length || length > m_bytes.size()) return 0;
        if (message.SetBuffer(m_bytes.data(), length) != length) return -1;
        m_bytes.erase(m_bytes.begin(), m_bytes.begin() + length);
        return 1;
    }
    size_t PendingBytes() const { return m_bytes.size(); }
    void Clear() { m_bytes.clear(); }
private:
    std::vector<char> m_bytes;
};
#endif
