#include "../RemoteCtrolSystem/SocketIPv4/TCPConnection.h"
#include "../RemoteCtrolSystem/SocketIPv4/TCPClient.h"
#include "../RemoteCtrolSystem/SocketIPv4/TCPThreadedServer.h"
#include "../RemoteCtrolSystem/Message/MessageFramer.h"
#include "../RemoteCtrolSystem/Message/SetupMsg.h"
#include "../RemoteCtrolSystem/Message/UserMsg.h"
#include "../RemoteCtrolSystem/Message/ImageMsg.h"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <memory>
#include <cstring>

namespace {
void Check(bool condition, const char* description) { if (!condition) throw std::runtime_error(description); }
std::vector<char> Bytes(ByteArrayMsg& message) { return {message.GetBufferPtr(), message.GetBufferPtr() + message.GetSize()}; }
class TestConnection : public TCPConnection {
public:
    void Connect(unsigned short port) {
        m_socket.Open(TCPSocket::TCP);
        m_socket.Connect(TCPSocketAddress("127.0.0.1", port));
        SetConnected(true);
    }
    void Fragment(const std::vector<char>& bytes, size_t step) {
        for (size_t offset = 0; offset < bytes.size();) {
            size_t count = bytes.size() - offset;
            if (count > step) count = step;
            int sent = m_socket.Send(bytes.data() + offset, count);
            Check(sent > 0, "fragment send"); offset += sent;
        }
    }
};
void FramingTests() {
    SetupMsg setup; setup.PacketMsg(RCS_SETUP_MSG_PIXEL, 1280, 720);
    ImageMsg image; RECT rect = {0, 0, 1, 1}; char jpeg[] = "JPEG payload";
    Check(image.PacketMsg(rect, sizeof(jpeg), jpeg), "image packet");
    UserMsg user; char name[] = "student", password[] = "test";
    Check(user.PacketMsg(name, password), "short credential input");
    std::vector<char> stream;
    for (auto message : {static_cast<ByteArrayMsg*>(&setup), static_cast<ByteArrayMsg*>(&image), static_cast<ByteArrayMsg*>(&user)}) {
        auto bytes = Bytes(*message); stream.insert(stream.end(), bytes.begin(), bytes.end());
    }
    // Every possible split includes split headers, payloads, and coalesced messages.
    for (size_t split = 0; split <= stream.size(); ++split) {
        MessageFramer framer; ByteArrayMsg message; int count = 0;
        Check(framer.Append(stream.data(), split), "first fragment");
        while (framer.Pop(message) == 1) ++count;
        Check(framer.Append(stream.data() + split, stream.size() - split), "second fragment");
        while (framer.Pop(message) == 1) ++count;
        Check(count == 3 && framer.PendingBytes() == 0, "fragmented/coalesced stream");
    }
    MessageFramer first, second; ByteArrayMsg result;
    Check(first.Append(stream.data(), 1), "partial first connection");
    Check(second.Append(setup.GetBufferPtr(), setup.GetSize()), "second connection");
    Check(second.Pop(result) == 1 && first.Pop(result) == 0, "independent connection state");
    Check(ByteArrayMsg::GetSize(image.GetBufferPtr(), 20) == 0, "short image header");
    Check(ByteArrayMsg::GetSize(image.GetBufferPtr(), 21) == image.GetSize(), "exact image header");
    auto malformed = Bytes(image); uint32_t size = 0xffffffffu;
    memcpy(malformed.data() + 17, &size, 4);
    Check(ByteArrayMsg::GetSize(malformed.data(), 21) == ByteArrayMsg::INVALID_SIZE, "oversized image rejected");
    char unknown = 127;
    Check(ByteArrayMsg::GetSize(&unknown, 1) == ByteArrayMsg::INVALID_SIZE, "unknown type rejected");
    Check(result.SetBuffer(setup.GetBufferPtr(), 12) == 0, "truncated copy rejected");
    char unterminated[20]; memset(unterminated, 'x', sizeof(unterminated));
    Check(!user.PacketMsg(unterminated, password), "overlong credential rejected");
    auto rawUser = Bytes(user); memset(rawUser.data() + 1, 'x', 20);
    Check(user.SetBuffer(rawUser.data(), rawUser.size()) == 41, "raw user frame");
    char output[20] = {};
    Check(!user.GetUsername(output) && output[19] == 0, "incoming credential termination");
}
void SocketTests() {
    TCPServer listener; listener.Start(0);
    TestConnection client; client.Connect(listener.GetAddress().GetPort());
    TCPSocketAddress address;
    std::unique_ptr<TCPConnection> server(listener.GetConnection(address));
    SetupMsg setup; setup.PacketMsg(RCS_SETUP_MSG_FRAME, 15);
    auto bytes = Bytes(setup);
    std::thread sender([&] { client.Fragment(bytes, 1); client.Fragment(bytes, bytes.size()); client.disconnect(); });
    ByteArrayMsg received;
    Check(server->receive(received) == 13 && !memcmp(received.GetBufferPtr(), bytes.data(), bytes.size()), "fragmented socket message");
    Check(server->receive(received) == 13, "coalesced/next socket message");
    Check(server->receive(received) == 0 && !server->IsConnected(), "orderly peer shutdown");
    sender.join();

    TestConnection truncated; truncated.Connect(listener.GetAddress().GetPort());
    server.reset(listener.GetConnection(address));
    truncated.Fragment({bytes.begin(), bytes.begin() + 5}, 5); truncated.disconnect();
    Check(server->receive(received) < 0, "partial message at disconnect rejected");

    TestConnection waiting; waiting.Connect(listener.GetAddress().GetPort());
    server.reset(listener.GetConnection(address));
    int result = 1;
    std::thread receiver([&] { ByteArrayMsg message; result = server->receive(message); });
    Sleep(20); server->disconnect(); receiver.join();
    Check(result <= 0, "local shutdown unblocks receiver");

    TestConnection large; large.Connect(listener.GetAddress().GetPort());
    server.reset(listener.GetConnection(address));
    std::vector<char> payload(1024 * 1024, 'j'); RECT rect = {0, 0, 1920, 1080};
    ImageMsg image; image.PacketMsg(rect, payload.size(), payload.data());
    int sent = 0;
    std::thread largeSender([&] { sent = large.send(image); });
    Check(server->receive(received) == static_cast<int>(image.GetSize()), "large frame receive");
    largeSender.join();
    Check(sent == static_cast<int>(image.GetSize()) && !memcmp(received.GetBufferPtr(), image.GetBufferPtr(), image.GetSize()), "complete large frame send");
}
void ClientHandshakeTests() {
    TCPServer listener; listener.Start(0);
    bool serverOK = false;
    std::thread server([&] {
        TCPSocketAddress address;
        std::unique_ptr<TCPConnection> peer(listener.GetConnection(address));
        SetupMsg ip; ip.PacketMsg(RCS_SETUP_MSG_PERMISSION, RCS_PERMISSION_IP, 1); peer->send(ip);
        UserMsg login;
        if (peer->receive(login) != 41 || login.GetType() != RCS_MSG_USER) return;
        SetupMsg permission; permission.PacketMsg(RCS_SETUP_MSG_PERMISSION, RCS_PERMISSION_CONTROL, 0);
        SetupMsg dimensions; dimensions.PacketMsg(RCS_SETUP_MSG_PIXEL, 800, 600);
        auto combined = Bytes(permission); auto next = Bytes(dimensions);
        combined.insert(combined.end(), next.begin(), next.end());
        serverOK = peer->send(combined.data(), static_cast<int>(combined.size())) == static_cast<int>(combined.size());
        peer->disconnect();
    });
    std::queue<ByteArrayMsg*> outgoing, incoming;
    TCPClient client;
    auto login = new UserMsg; char name[] = "student", password[] = "test"; login->PacketMsg(name, password); outgoing.push(login);
    Check(client.Init("127.0.0.1", listener.GetAddress().GetPort(), outgoing, incoming) == 0, "client handshake");
    client.Start();
    ByteArrayMsg* message = NULL;
    for (int i = 0; i < 200 && !client.PopReceived(message); ++i) Sleep(5);
    Check(message && message->GetType() == RCS_MSG_SETUP && message->GetSize() == 13, "handshake leftovers reach receive queue");
    delete message;
    client.Close(); server.join();
    Check(serverOK && incoming.empty() && outgoing.empty(), "client shutdown and queue cleanup");
}
class WaitingService : public TCPService {
public:
    explicit WaitingService(TCPConnection* peer) : TCPService(peer) {}
    void Run() override { ByteArrayMsg message; connection.receive(message); }
};
void ServerLifetimeTests() {
    TCPServiceFactoryTemplate<WaitingService> factory;
    TCPThreadedServer server;
    for (int round = 0; round < 3; ++round) {
        server.Start(factory, 0);
        TestConnection client; client.Connect(server.GetAddress().GetPort());
        for (int i = 0; i < 100 && !server.ProcessedClients(); ++i) Sleep(5);
        Check(server.ProcessedClients() == 1, "service accepted");
        server.Stop(); // Must close and join the blocked service, then free it.
        Check(!server.ProcessedClients() || server.ProcessedClients() == 1, "server stop");
    }
}
}
int main() {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 2;
    try {
        FramingTests(); SocketTests(); ClientHandshakeTests(); ServerLifetimeTests();
        WSACleanup(); std::cout << "Protocol framing, sockets, handshake and shutdown tests passed.\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; WSACleanup(); return 1; }
}
