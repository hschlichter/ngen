#pragma once

#include <cstdint>
#include <expected>
#include <string>

// TCP on loopback only: every RPC endpoint listens on 127.0.0.1 with a port the OS picks.
namespace rpc {

// A listening socket on 127.0.0.1:port (0 = any free port). Returns the fd and writes the
// bound port.
auto listenLoopback(uint16_t port, uint16_t& boundPort) -> std::expected<int, std::string>;
// A blocking connection to 127.0.0.1:port, with TCP_NODELAY.
auto connectLoopback(uint16_t port) -> std::expected<int, std::string>;
auto setNonBlocking(int fd) -> bool;
auto closeSocket(int fd) -> void;

} // namespace rpc
