#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

// TCP on loopback only: every RPC endpoint listens on 127.0.0.1 with a port the OS picks. The sockets the RPC core uses, one
// implementation per OS (rpcsocketlinux.cpp, rpcsocketwindows.cpp): BSD sockets with poll() and an eventfd on Linux, Winsock with
// WSAPoll() and a loopback socket pair on Windows.
namespace rpc {

// A socket: a file descriptor on Linux, a SOCKET on Windows.
using Socket = intptr_t;
inline constexpr Socket invalidSocket = -1;

// A listening socket on 127.0.0.1:port (0 = any free port). Returns the socket and writes the bound port.
auto listenLoopback(uint16_t port, uint16_t& boundPort) -> std::expected<Socket, std::string>;
// A blocking connection to 127.0.0.1:port, with TCP_NODELAY.
auto connectLoopback(uint16_t port) -> std::expected<Socket, std::string>;
// The next waiting connection on a non-blocking listener, itself non-blocking; invalidSocket when none waits.
auto acceptConnection(Socket listener) -> Socket;
auto setNonBlocking(Socket socket) -> bool;
auto closeSocket(Socket socket) -> void;

// Bytes sent or received. Receiving 0 is the peer closing. -1 is an error: lastErrorWouldBlock() tells a full or empty buffer on a
// non-blocking socket, and lastErrorInterrupted() a call to repeat, from a broken connection. Sending never raises SIGPIPE.
auto sendSome(Socket socket, const std::byte* data, size_t size) -> ptrdiff_t;
auto receiveSome(Socket socket, std::byte* data, size_t size) -> ptrdiff_t;
auto lastErrorWouldBlock() -> bool;
auto lastErrorInterrupted() -> bool;
auto lastErrorText() -> std::string;

struct PollEntry {
    Socket socket = invalidSocket;
    bool wantRead = false;
    bool wantWrite = false;
    // Results.
    bool readable = false;
    bool writable = false;
    bool closed = false; // hung up or in error: a receive tells which
};

// Waits until an entry is ready or timeoutMs passes. Returns the number of ready entries, 0 on timeout, -1 on error (lastErrorInterrupted()
// for a wait to repeat).
auto pollSockets(std::span<PollEntry> entries, int timeoutMs) -> int;

// Wakes a thread waiting in pollSockets() from another thread: its socket() becomes readable on signal() until drain(). An eventfd on
// Linux; on Windows, where WSAPoll() takes sockets only, a connected loopback socket pair.
class Waker {
public:
    Waker() = default;
    Waker(const Waker&) = delete;
    auto operator=(const Waker&) -> Waker& = delete;
    ~Waker();

    auto open() -> bool;
    auto close() -> void;
    auto signal() -> void;
    auto drain() -> void;
    auto socket() const -> Socket { return readEnd; }
    auto isOpen() const -> bool { return readEnd != invalidSocket; }

private:
    Socket readEnd = invalidSocket;
    Socket writeEnd = invalidSocket;
};

} // namespace rpc
