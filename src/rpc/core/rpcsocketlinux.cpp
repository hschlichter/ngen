#include "rpcsocket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace rpc {

namespace {

auto loopbackAddress(uint16_t port) -> sockaddr_in {
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

auto noDelay(int fd) -> void {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

} // namespace

auto listenLoopback(uint16_t port, uint16_t& boundPort) -> std::expected<Socket, std::string> {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return std::unexpected(std::format("socket: {}", std::strerror(errno)));
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    auto address = loopbackAddress(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        auto message = std::format("bind 127.0.0.1:{}: {}", port, std::strerror(errno));
        close(fd);
        return std::unexpected(message);
    }
    if (listen(fd, 16) != 0) {
        auto message = std::format("listen: {}", std::strerror(errno));
        close(fd);
        return std::unexpected(message);
    }
    sockaddr_in bound = {};
    socklen_t length = sizeof(bound);
    getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &length);
    boundPort = ntohs(bound.sin_port);
    return fd;
}

auto connectLoopback(uint16_t port) -> std::expected<Socket, std::string> {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return std::unexpected(std::format("socket: {}", std::strerror(errno)));
    }
    auto address = loopbackAddress(port);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        auto message = std::format("connect 127.0.0.1:{}: {}", port, std::strerror(errno));
        close(fd);
        return std::unexpected(message);
    }
    noDelay(fd);
    return fd;
}

auto acceptConnection(Socket listener) -> Socket {
    int fd = accept4((int) listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    return fd < 0 ? invalidSocket : fd;
}

auto setNonBlocking(Socket socket) -> bool {
    int flags = fcntl((int) socket, F_GETFL, 0);
    return flags >= 0 && fcntl((int) socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

auto closeSocket(Socket socket) -> void {
    if (socket >= 0) {
        close((int) socket);
    }
}

auto sendSome(Socket socket, const std::byte* data, size_t size) -> ptrdiff_t {
    return ::send((int) socket, data, size, MSG_NOSIGNAL);
}

auto receiveSome(Socket socket, std::byte* data, size_t size) -> ptrdiff_t {
    return ::recv((int) socket, data, size, 0);
}

auto lastErrorWouldBlock() -> bool {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

auto lastErrorInterrupted() -> bool {
    return errno == EINTR;
}

auto lastErrorText() -> std::string {
    return std::strerror(errno);
}

auto pollSockets(std::span<PollEntry> entries, int timeoutMs) -> int {
    std::vector<pollfd> fds;
    fds.reserve(entries.size());
    for (const auto& entry : entries) {
        short events = 0;
        if (entry.wantRead) {
            events |= POLLIN;
        }
        if (entry.wantWrite) {
            events |= POLLOUT;
        }
        fds.push_back({.fd = (int) entry.socket, .events = events, .revents = 0});
    }
    int polled = poll(fds.data(), (nfds_t) fds.size(), timeoutMs);
    for (size_t i = 0; i < entries.size(); i++) {
        auto revents = polled > 0 ? fds[i].revents : 0;
        entries[i].readable = (revents & POLLIN) != 0;
        entries[i].writable = (revents & POLLOUT) != 0;
        entries[i].closed = (revents & (POLLHUP | POLLERR)) != 0;
    }
    return polled;
}

Waker::~Waker() {
    close();
}

auto Waker::open() -> bool {
    close();
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    readEnd = fd;
    writeEnd = fd;
    return true;
}

auto Waker::close() -> void {
    if (readEnd >= 0) {
        ::close((int) readEnd);
    }
    readEnd = invalidSocket;
    writeEnd = invalidSocket;
}

auto Waker::signal() -> void {
    if (writeEnd >= 0) {
        uint64_t one = 1;
        [[maybe_unused]] auto written = write((int) writeEnd, &one, sizeof(one));
    }
}

auto Waker::drain() -> void {
    if (readEnd >= 0) {
        uint64_t value = 0;
        [[maybe_unused]] auto consumed = read((int) readEnd, &value, sizeof(value));
    }
}

} // namespace rpc
