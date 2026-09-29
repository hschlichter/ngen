#include "rpcclient.h"

#include "rpcframe.h"
#include "rpcsocket.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>

struct RpcClient::State {
    int fd = -1;
    RpcFrameReader reader;
    int64_t nextId = 1;
};

RpcClient::RpcClient() : state(std::make_unique<State>()) {
}

RpcClient::~RpcClient() {
    close();
}

auto RpcClient::connect(uint16_t port) -> std::expected<void, std::string> {
    auto fd = rpc::connectLoopback(port);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    state->fd = *fd;
    return {};
}

auto RpcClient::close() -> void {
    rpc::closeSocket(state->fd);
    state->fd = -1;
}

auto RpcClient::call(std::string_view method, const rpc::Json& params, std::span<const std::byte> attachment) -> std::expected<Response, std::string> {
    if (state->fd < 0) {
        return std::unexpected("not connected");
    }
    auto id = state->nextId++;
    auto bytes = encodeRpcFrame(rpc::makeRequest(id, method, params).dump(), attachment);
    size_t sent = 0;
    while (sent < bytes.size()) {
        auto n = ::send(state->fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return std::unexpected(std::string("send: ") + std::strerror(errno));
        }
        sent += (size_t) n;
    }
    std::array<std::byte, 64 * 1024> buffer = {};
    while (true) {
        std::string error;
        while (auto frame = state->reader.next(error)) {
            auto message = rpc::Json::parse(frame->json, nullptr, false);
            if (message.is_discarded()) {
                return std::unexpected("invalid JSON from peer");
            }
            auto kind = rpc::messageKind(message);
            if (kind == rpc::MessageKind::Request) {
                auto reply = encodeRpcFrame(rpc::makeError(message["id"], rpc::methodNotFound, "this client serves no methods").dump(), {});
                ::send(state->fd, reply.data(), reply.size(), MSG_NOSIGNAL);
                continue;
            }
            if (kind == rpc::MessageKind::Response && message["id"] == id) {
                return Response{.message = std::move(message), .attachment = std::move(frame->attachment)};
            }
        }
        if (!error.empty()) {
            return std::unexpected(error);
        }
        auto received = recv(state->fd, buffer.data(), buffer.size(), 0);
        if (received == 0) {
            return std::unexpected("connection closed before the response arrived");
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(std::string("recv: ") + std::strerror(errno));
        }
        state->reader.append(std::span(buffer.data(), (size_t) received));
    }
}
