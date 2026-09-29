#include "rpcsocket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

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

auto listenLoopback(uint16_t port, uint16_t& boundPort) -> std::expected<int, std::string> {
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

auto connectLoopback(uint16_t port) -> std::expected<int, std::string> {
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

auto setNonBlocking(int fd) -> bool {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

auto closeSocket(int fd) -> void {
    if (fd >= 0) {
        close(fd);
    }
}

} // namespace rpc
