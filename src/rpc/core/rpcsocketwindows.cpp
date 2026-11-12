#include "rpcsocket.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <climits>
#include <format>
#include <mutex>
#include <vector>

namespace rpc {

namespace {

// Winsock is started once per process, the first time a socket is made, and stays up until exit.
auto startWinsock() -> void {
    static std::once_flag started;
    std::call_once(started, [] {
        WSADATA data = {};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

auto loopbackAddress(uint16_t port) -> sockaddr_in {
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

auto noDelay(SOCKET socket) -> void {
    BOOL one = TRUE;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

auto errorText(int code) -> std::string {
    char* buffer = nullptr;
    DWORD length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr,
                                  (DWORD) code,
                                  0,
                                  reinterpret_cast<char*>(&buffer),
                                  0,
                                  nullptr);
    std::string text = length > 0 ? std::string(buffer, length) : std::format("error {}", code);
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
        text.pop_back();
    }
    return text;
}

// A socket made non-inheritable, so a child process (a packer) does not hold the RPC connections open.
auto newSocket() -> SOCKET {
    startWinsock();
    return WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

} // namespace

auto listenLoopback(uint16_t port, uint16_t& boundPort) -> std::expected<Socket, std::string> {
    SOCKET socket = newSocket();
    if (socket == INVALID_SOCKET) {
        return std::unexpected(std::format("socket: {}", errorText(WSAGetLastError())));
    }
    // Windows' SO_REUSEADDR lets another socket steal a bound port; exclusive use is the Linux meaning.
    BOOL one = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
    auto address = loopbackAddress(port);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        auto message = std::format("bind 127.0.0.1:{}: {}", port, errorText(WSAGetLastError()));
        closesocket(socket);
        return std::unexpected(message);
    }
    if (listen(socket, 16) != 0) {
        auto message = std::format("listen: {}", errorText(WSAGetLastError()));
        closesocket(socket);
        return std::unexpected(message);
    }
    sockaddr_in bound = {};
    int length = sizeof(bound);
    getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &length);
    boundPort = ntohs(bound.sin_port);
    return (Socket) socket;
}

auto connectLoopback(uint16_t port) -> std::expected<Socket, std::string> {
    SOCKET socket = newSocket();
    if (socket == INVALID_SOCKET) {
        return std::unexpected(std::format("socket: {}", errorText(WSAGetLastError())));
    }
    auto address = loopbackAddress(port);
    if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        auto message = std::format("connect 127.0.0.1:{}: {}", port, errorText(WSAGetLastError()));
        closesocket(socket);
        return std::unexpected(message);
    }
    noDelay(socket);
    return (Socket) socket;
}

auto acceptConnection(Socket listener) -> Socket {
    SOCKET socket = accept((SOCKET) listener, nullptr, nullptr);
    if (socket == INVALID_SOCKET) {
        return invalidSocket;
    }
    SetHandleInformation((HANDLE) socket, HANDLE_FLAG_INHERIT, 0);
    setNonBlocking((Socket) socket);
    return (Socket) socket;
}

auto setNonBlocking(Socket socket) -> bool {
    u_long one = 1;
    return ioctlsocket((SOCKET) socket, FIONBIO, &one) == 0;
}

auto closeSocket(Socket socket) -> void {
    if (socket != invalidSocket) {
        closesocket((SOCKET) socket);
    }
}

auto sendSome(Socket socket, const std::byte* data, size_t size) -> ptrdiff_t {
    int sent = ::send((SOCKET) socket, reinterpret_cast<const char*>(data), (int) std::min<size_t>(size, INT_MAX), 0);
    return sent == SOCKET_ERROR ? -1 : sent;
}

auto receiveSome(Socket socket, std::byte* data, size_t size) -> ptrdiff_t {
    int received = ::recv((SOCKET) socket, reinterpret_cast<char*>(data), (int) std::min<size_t>(size, INT_MAX), 0);
    return received == SOCKET_ERROR ? -1 : received;
}

auto lastErrorWouldBlock() -> bool {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

auto lastErrorInterrupted() -> bool {
    return WSAGetLastError() == WSAEINTR;
}

auto lastErrorText() -> std::string {
    return errorText(WSAGetLastError());
}

auto pollSockets(std::span<PollEntry> entries, int timeoutMs) -> int {
    std::vector<WSAPOLLFD> fds;
    fds.reserve(entries.size());
    for (const auto& entry : entries) {
        SHORT events = 0;
        if (entry.wantRead) {
            events |= POLLRDNORM;
        }
        if (entry.wantWrite) {
            events |= POLLWRNORM;
        }
        fds.push_back({.fd = (SOCKET) entry.socket, .events = events, .revents = 0});
    }
    int polled = WSAPoll(fds.data(), (ULONG) fds.size(), timeoutMs);
    for (size_t i = 0; i < entries.size(); i++) {
        auto revents = polled > 0 ? fds[i].revents : 0;
        entries[i].readable = (revents & POLLRDNORM) != 0;
        entries[i].writable = (revents & POLLWRNORM) != 0;
        entries[i].closed = (revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
    }
    return polled == SOCKET_ERROR ? -1 : polled;
}

Waker::~Waker() {
    close();
}

auto Waker::open() -> bool {
    close();
    // A connected pair through a listener on an OS-picked loopback port, closed once the pair is made.
    uint16_t port = 0;
    auto listener = listenLoopback(0, port);
    if (!listener) {
        return false;
    }
    auto writer = connectLoopback(port);
    if (!writer) {
        closeSocket(*listener);
        return false;
    }
    SOCKET reader = accept((SOCKET) *listener, nullptr, nullptr);
    closeSocket(*listener);
    if (reader == INVALID_SOCKET) {
        closeSocket(*writer);
        return false;
    }
    SetHandleInformation((HANDLE) reader, HANDLE_FLAG_INHERIT, 0);
    readEnd = (Socket) reader;
    writeEnd = *writer;
    setNonBlocking(readEnd);
    setNonBlocking(writeEnd);
    return true;
}

auto Waker::close() -> void {
    closeSocket(readEnd);
    closeSocket(writeEnd);
    readEnd = invalidSocket;
    writeEnd = invalidSocket;
}

auto Waker::signal() -> void {
    if (writeEnd != invalidSocket) {
        // One byte is enough; a full buffer already holds a wake-up.
        char one = 1;
        ::send((SOCKET) writeEnd, &one, 1, 0);
    }
}

auto Waker::drain() -> void {
    if (readEnd == invalidSocket) {
        return;
    }
    char buffer[256];
    while (::recv((SOCKET) readEnd, buffer, sizeof(buffer), 0) > 0) {
    }
}

} // namespace rpc
