#pragma once

#include "rpcprotocol.h"

// The client is for command-line tools, which all handle JSON; its response carries a full value.
#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

// A blocking client connection, for command-line tools. call() sends one request and waits
// for its response; requests the peer sends meanwhile are answered with methodNotFound.
class RpcClient {
public:
    struct Response {
        rpc::Json message; // the whole response: "result" or "error"
        std::vector<std::byte> attachment;
    };

    RpcClient();
    RpcClient(const RpcClient&) = delete;
    auto operator=(const RpcClient&) -> RpcClient& = delete;
    ~RpcClient();

    auto connect(uint16_t port) -> std::expected<void, std::string>;
    auto call(std::string_view method, const rpc::Json& params, std::span<const std::byte> attachment = {}) -> std::expected<Response, std::string>;
    auto close() -> void;

private:
    struct State;
    std::unique_ptr<State> state;
};
