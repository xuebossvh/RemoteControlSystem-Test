#pragma once
#include "../Message/MessageData.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <new>

// Bounded, owning queue for the Client's small post-login messages. The caller
// holds its mutex. Only adjacent pure moves can replace one another: buttons,
// keys and configuration packets are strict ordering barriers.
class ClientSendQueue {
public:
    struct Packet {
        std::array<char, 13> bytes{};
        std::size_t size = 0;
        bool movement = false;
    };
    explicit ClientSendQueue(std::size_t capacity = 512) : capacity_(capacity) {}
    bool Push(const void* data, std::size_t size) {
        if (!data || size > 13 || size == 0) return false;
        const unsigned char type = *static_cast<const unsigned char*>(data);
        if (!((type == RCS_MSG_MOUSE && size == 13) ||
              (type == RCS_MSG_KEYBOARD && size == 9) ||
              (type == RCS_MSG_SETUP && size == 13))) return false;
        Packet packet;
        std::memcpy(packet.bytes.data(), data, size);
        packet.size = size;
        std::int32_t status = -1;
        if (type == RCS_MSG_MOUSE) std::memcpy(&status, packet.bytes.data() + 9, sizeof(status));
        packet.movement = type == RCS_MSG_MOUSE && status == 0;
        if (packet.movement && !packets_.empty() && packets_.back().movement) {
            packets_.back() = packet;
            return true;
        }
        if (packets_.size() >= capacity_) return false;
        try { packets_.push_back(packet); return true; }
        catch (const std::bad_alloc&) { return false; }
    }
    bool Pop(Packet& packet) {
        if (packets_.empty()) return false;
        packet = packets_.front(); packets_.pop_front(); return true;
    }
    bool Empty() const { return packets_.empty(); }
    std::size_t Size() const { return packets_.size(); }
    bool FrontIsMovement() const { return !packets_.empty() && packets_.front().movement; }
    void Clear() { packets_.clear(); }
private:
    std::size_t capacity_;
    std::deque<Packet> packets_;
};
