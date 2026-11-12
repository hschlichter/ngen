#include "rpcclient.h"

#include "rpcframe.h"
#include "rpcsocket.h"

#include <nlohmann/json.hpp>

#include <array>

struct RpcClient::State {
    rpc::Socket fd = rpc::invalidSocket;
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
    state->fd = rpc::invalidSocket;
}

auto RpcClient::call(std::string_view method, const rpc::Json& params, std::span<const std::byte> attachment) -> std::expected<Response, std::string> {
    if (state->fd == rpc::invalidSocket) {
        return std::unexpected("not connected");
    }
    auto id = state->nextId++;
    auto bytes = encodeRpcFrame(rpc::makeRequest(id, method, params).dump(), attachment);
    size_t sent = 0;
    while (sent < bytes.size()) {
        auto n = rpc::sendSome(state->fd, bytes.data() + sent, bytes.size() - sent);
        if (n <= 0) {
            return std::unexpected(std::string("send: ") + rpc::lastErrorText());
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
                rpc::sendSome(state->fd, reply.data(), reply.size());
                continue;
            }
            if (kind == rpc::MessageKind::Response && message["id"] == id) {
                return Response{.message = std::move(message), .attachment = std::move(frame->attachment)};
            }
        }
        if (!error.empty()) {
            return std::unexpected(error);
        }
        auto received = rpc::receiveSome(state->fd, buffer.data(), buffer.size());
        if (received == 0) {
            return std::unexpected("connection closed before the response arrived");
        }
        if (received < 0) {
            if (rpc::lastErrorInterrupted()) {
                continue;
            }
            return std::unexpected(std::string("recv: ") + rpc::lastErrorText());
        }
        state->reader.append(std::span(buffer.data(), (size_t) received));
    }
}
