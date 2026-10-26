#ifndef __BYTEARRAYMSG_H__
#define __BYTEARRAYMSG_H__
#include <cstddef>
#include <cstdint>
#include "MessageData.h"

// Legacy little-endian wire format: image header is always 21 bytes.
class ByteArrayMsg : public MessageData {
public:
    static const size_t MAX_IMAGE_BYTES = 16u * 1024u * 1024u;
    static const size_t INVALID_SIZE = static_cast<size_t>(-1);
    ByteArrayMsg();
    virtual ~ByteArrayMsg();
    virtual unsigned __int8 GetType();
    virtual size_t GetBuffer(void* buffer, size_t len);
    virtual size_t SetBuffer(const void* buffer);
    size_t SetBuffer(const void* buffer, size_t available);
    virtual char* GetBufferPtr();
    virtual size_t GetSize();
    // 0: incomplete header; INVALID_SIZE: unsupported type/invalid length.
    static size_t GetSize(const void* buffer, int available);
protected:
    char* m_buffer;
    size_t m_size;
private:
    ByteArrayMsg(const ByteArrayMsg&) = delete;
    ByteArrayMsg& operator=(const ByteArrayMsg&) = delete;
};
#endif
