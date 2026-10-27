#include "../TCPClient.h"
#include "../TCPServer.h"
#include "../../Message/MouseMsg.h"
#include "../../Message/KeyboardMsg.h"
#include "../../Message/SetupMsg.h"
#include "../../Message/UserMsg.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
void Check(bool value, const char* description) { if (!value) throw std::runtime_error(description); }
long long MillisecondsSince(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}
template<class Predicate> void Wait(Predicate predicate, const char* description) {
    const auto start = Clock::now();
    while (!predicate()) {
        if (MillisecondsSince(start) > 3000) throw std::runtime_error(description);
        Sleep(1);
    }
}
struct Event { int type = 0, a = 0, b = 0, c = 0; };
Event Decode(const char* bytes, size_t size) {
    Event event; event.type = static_cast<unsigned char>(bytes[0]);
    if (size >= 5) std::memcpy(&event.a, bytes + 1, 4);
    if (size >= 9) std::memcpy(&event.b, bytes + 5, 4);
    if (size >= 13) std::memcpy(&event.c, bytes + 9, 4);
    return event;
}
class Fixture {
public:
    explicit Fixture(bool pauseReading = false) : paused_(pauseReading) {
        listener_.Start(0);
        port_ = listener_.GetAddress().GetPort();
        thread_ = std::thread([this] { Run(); });
    }
    ~Fixture() {
        stopping_ = true;
        listener_.Stop();
        std::shared_ptr<TCPConnection> peer;
        { std::lock_guard<std::mutex> lock(mutex_); peer = peer_; }
        if (peer) peer->disconnect();
        changed_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    unsigned short Port() const { return port_; }
    unsigned short ClientPort() const { return clientPort_.load(); }
    std::vector<Event> Events() { std::lock_guard<std::mutex> lock(mutex_); return events_; }
    bool Failed() { std::lock_guard<std::mutex> lock(mutex_); return failed_; }
private:
    void Run() {
        try {
            TCPSocketAddress address;
            std::shared_ptr<TCPConnection> peer(listener_.GetConnection(address));
            { std::lock_guard<std::mutex> lock(mutex_); peer_ = peer; }
            clientPort_ = peer->getConnectedAddress().GetPort();
            SetupMsg response;
            response.PacketMsg(RCS_SETUP_MSG_PERMISSION, RCS_PERMISSION_IP, 1);
            Check(peer->send(response) == 13, "fixture IP permission");
            UserMsg login;
            Check(peer->receive(login) == 41 && login.GetType() == RCS_MSG_USER, "fixture login");
            response.PacketMsg(RCS_SETUP_MSG_PERMISSION, RCS_PERMISSION_CONTROL, 1);
            Check(peer->send(response) == 13, "fixture control permission");
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [this] { return stopping_ || !paused_; });
            }
            ByteArrayMsg incoming;
            while (!stopping_ && peer->receive(incoming) > 0) {
                const Event event = Decode(incoming.GetBufferPtr(), incoming.GetSize());
                std::lock_guard<std::mutex> lock(mutex_);
                events_.push_back(event);
            }
        } catch (...) { std::lock_guard<std::mutex> lock(mutex_); if (!stopping_) failed_ = true; }
    }
    TCPServer listener_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<unsigned short> clientPort_{0};
    bool paused_ = false, failed_ = false;
    unsigned short port_ = 0;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::shared_ptr<TCPConnection> peer_;
    std::vector<Event> events_;
};
class ClientProbe : public TCPClient {
public:
    int NoDelay() {
        int value = 0, length = sizeof(value);
        Check(getsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&value), &length) == 0,
              "read TCP_NODELAY");
        return value;
    }
    void SmallSendBuffer() { Check(m_socket.SetSendBufLength(1024) == 0, "reduce test send buffer"); }
};
void Login(ClientProbe& client, Fixture& fixture, std::queue<ByteArrayMsg*>& outgoing,
           std::queue<ByteArrayMsg*>& incoming) {
    std::unique_ptr<UserMsg> login(new UserMsg);
    char name[] = "test", password[] = "test";
    Check(login->PacketMsg(name, password), "prepare login");
    outgoing.push(login.release());
    Check(client.Init("127.0.0.1", fixture.Port(), outgoing, incoming) == 1, "client handshake");
    Check(client.NoDelay() == 1, "input socket disables Nagle");
    Check(client.getOwnAddress().GetPort() == fixture.ClientPort(), "own address identifies peer endpoint");
    client.Start();
}
void QueueTests() {
    ClientSendQueue queue(3);
    MouseMsg move, button;
    KeyboardMsg key;
    move.PacketMsg(10, 20, 0); Check(queue.Push(move.GetBufferPtr(), move.GetSize()), "first move queued");
    move.PacketMsg(30, 40, 0); Check(queue.Push(move.GetBufferPtr(), move.GetSize()), "second move coalesced");
    Check(queue.Size() == 1, "adjacent moves occupy one slot");
    button.PacketMsg(30, 40, RCS_MOUSE_LEFT_DOWN);
    Check(queue.Push(button.GetBufferPtr(), button.GetSize()), "button is an ordering barrier");
    move.PacketMsg(50, 60, 0); Check(queue.Push(move.GetBufferPtr(), move.GetSize()), "move after button");
    move.PacketMsg(70, 80, 0); Check(queue.Push(move.GetBufferPtr(), move.GetSize()), "latest move replaces tail at capacity");
    key.PacketMsg(65, RCS_KEY_UP);
    Check(!queue.Push(key.GetBufferPtr(), key.GetSize()) && queue.Size() == 3, "full queue reports failure without silent key loss");
    ClientSendQueue::Packet packet;
    Check(queue.Pop(packet) && packet.movement && Decode(packet.bytes.data(), packet.size).a == 30, "latest pre-click position retained");
    Check(queue.Pop(packet) && !packet.movement && Decode(packet.bytes.data(), packet.size).c == RCS_MOUSE_LEFT_DOWN, "button order retained");
    Check(queue.Pop(packet) && packet.movement && Decode(packet.bytes.data(), packet.size).a == 70, "latest post-click position retained");
    Check(!queue.Pop(packet), "bounded queue drained");
    Check(!queue.Push(nullptr, 13) && !queue.Push(key.GetBufferPtr(), 8), "invalid frames rejected");
}
void OrderedInputTests() {
    Fixture fixture;
    std::queue<ByteArrayMsg*> outgoing, incoming;
    ClientProbe client;
    Login(client, fixture, outgoing, incoming);
    MouseMsg mouse; KeyboardMsg key; SetupMsg setup;
    const auto producerStart = Clock::now();
    for (int i = 0; i < 1000; ++i) {
        mouse.PacketMsg(i, 100, 0); Check(client.send(mouse) == 13, "first movement burst accepted");
    }
    mouse.PacketMsg(999, 100, RCS_MOUSE_LEFT_DOWN); Check(client.send(mouse) == 13, "button down accepted");
    key.PacketMsg(65, RCS_KEY_DOWN); Check(client.send(key) == 9, "key down accepted");
    for (int i = 1000; i < 2000; ++i) {
        mouse.PacketMsg(i, 200, 0); Check(client.send(mouse) == 13, "second movement burst accepted");
    }
    key.PacketMsg(65, RCS_KEY_UP); Check(client.send(key) == 9, "key up accepted");
    mouse.PacketMsg(1999, 200, RCS_MOUSE_LEFT_UP); Check(client.send(mouse) == 13, "button up accepted");
    setup.PacketMsg(RCS_SETUP_MSG_FRAME, 15); Check(client.send(setup) == 13, "setting accepted");
    const auto producerMilliseconds = MillisecondsSince(producerStart);
    Check(producerMilliseconds < 1000, "producer does not wait on movement pacing/network");
    const auto finalStart = Clock::now();
    mouse.PacketMsg(1777, 888, 0); Check(client.send(mouse) == 13, "final mouse position accepted");
    Wait([&] {
        auto events = fixture.Events();
        return !events.empty() && events.back().type == RCS_MSG_MOUSE &&
               events.back().a == 1777 && events.back().b == 888 && events.back().c == 0;
    }, "last movement was never flushed");
    const auto flushMilliseconds = MillisecondsSince(finalStart);
    Check(flushMilliseconds < 500, "final position has a bounded flush deadline");
    auto events = fixture.Events();
    std::vector<Event> barriers;
    for (size_t i = 0; i < events.size(); ++i) {
        const auto& event = events[i];
        if (event.type == RCS_MSG_MOUSE && event.c == 0) continue;
        barriers.push_back(event);
        if (event.type == RCS_MSG_MOUSE && event.c == RCS_MOUSE_LEFT_DOWN)
            Check(i && events[i - 1].type == RCS_MSG_MOUSE && events[i - 1].a == 999, "final move precedes button down");
        if (event.type == RCS_MSG_KEYBOARD && event.b == RCS_KEY_UP)
            Check(i && events[i - 1].type == RCS_MSG_MOUSE && events[i - 1].a == 1999, "drag movement precedes key up");
    }
    Check(barriers.size() == 5, "all critical packets delivered exactly once");
    Check(barriers[0].type == RCS_MSG_MOUSE && barriers[0].c == RCS_MOUSE_LEFT_DOWN &&
          barriers[1].type == RCS_MSG_KEYBOARD && barriers[1].b == RCS_KEY_DOWN &&
          barriers[2].type == RCS_MSG_KEYBOARD && barriers[2].b == RCS_KEY_UP &&
          barriers[3].type == RCS_MSG_MOUSE && barriers[3].c == RCS_MOUSE_LEFT_UP &&
          barriers[4].type == RCS_MSG_SETUP, "buttons/keys/settings preserve their exact order");
    Check(!fixture.Failed(), "fixture remains healthy");
    client.Close(); client.Close();
    Check(outgoing.empty() && incoming.empty(), "idempotent cleanup");
    std::cout << "ordered_input: producer_ms=" << producerMilliseconds
              << " final_move_ms=" << flushMilliseconds << " received_packets=" << events.size() << "\n";
}
void BackpressureTests() {
    Fixture fixture(true);
    std::queue<ByteArrayMsg*> outgoing, incoming;
    ClientProbe client;
    Login(client, fixture, outgoing, incoming);
    client.SmallSendBuffer();
    KeyboardMsg key;
    bool rejected = false;
    const auto start = Clock::now();
    for (int i = 0; i < 100000; ++i) {
        key.PacketMsg(65, i % 2 ? RCS_KEY_UP : RCS_KEY_DOWN);
        if (client.send(key) < 0) { rejected = true; break; }
    }
    Check(rejected && !client.IsConnected(), "critical queue overflow explicitly closes the session");
    Check(MillisecondsSince(start) < 1000, "backpressure never blocks the producer");
    const auto close = Clock::now(); client.Close();
    Check(MillisecondsSince(close) < 1000, "close interrupts a stalled sender and receiver");
}
void CloseRaceAndReconnectTests() {
    Fixture first(true);
    std::queue<ByteArrayMsg*> outgoing, incoming;
    ClientProbe client;
    Login(client, first, outgoing, incoming);
    std::atomic<bool> failed(false);
    std::thread producer([&] {
        try {
            MouseMsg message;
            for (int i = 0; i < 100000; ++i) {
                message.PacketMsg(i, 10, 0);
                if (client.send(message) < 0) break;
            }
        } catch (...) { failed = true; }
    });
    Sleep(10);
    const auto start = Clock::now(); client.Close(); producer.join();
    Check(MillisecondsSince(start) < 1000 && !failed, "concurrent producer/close finishes without deadlock");
    Fixture second;
    Login(client, second, outgoing, incoming);
    MouseMsg final; final.PacketMsg(42, 73, 0);
    Check(client.send(final) == 13, "reconnected client accepts new input");
    Wait([&] { return !second.Events().empty(); }, "reconnected sender failed");
    const auto events = second.Events();
    Check(events.size() == 1 && events[0].a == 42 && events[0].b == 73, "old queued input never leaks into a new session");
    client.Close();
}
}
int main() {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 2;
    // A lifetime regression can deadlock a fixture destructor before a normal
    // assertion reaches main. Bound the entire isolated test process as well.
    std::mutex deadlineMutex;
    std::condition_variable deadlineChanged;
    bool finished = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(deadlineMutex);
        if (!deadlineChanged.wait_for(lock, std::chrono::seconds(20), [&] { return finished; })) {
            std::cerr << "FAIL: client send regression exceeded its 20-second process deadline" << std::endl;
            TerminateProcess(GetCurrentProcess(), 3);
        }
    });
    int result = 0;
    try {
        std::cout << "queue_tests" << std::endl; QueueTests();
        std::cout << "ordered_input_tests" << std::endl; OrderedInputTests();
        std::cout << "backpressure_tests" << std::endl; BackpressureTests();
        std::cout << "close_race_reconnect_tests" << std::endl; CloseRaceAndReconnectTests();
        std::cout << "PASS: async input queue, TCP_NODELAY, movement tail, critical order, backpressure, close/reconnect (no OS input injected)\n";
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; result = 1; }
    {
        std::lock_guard<std::mutex> lock(deadlineMutex);
        finished = true;
    }
    deadlineChanged.notify_one();
    watchdog.join();
    WSACleanup();
    return result;
}
