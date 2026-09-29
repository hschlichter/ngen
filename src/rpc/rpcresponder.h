#pragma once

#include "rpcprotocol.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The answer to one call. Copies share state: the first respond() or fail() wins and later
// ones are ignored, so a handler can keep a copy and complete it frames later (a screenshot
// after its fence, a capture when its result arrives).
struct RpcReply {
    bool ok = true;
    const rpc::Json* result = nullptr; // valid while the deliver callback runs
    int code = 0;
    std::string message;
    std::vector<std::byte> attachment;
};

class RpcResponder {
public:
    using Deliver = std::function<void(const RpcReply& reply)>;

    RpcResponder() = default;
    explicit RpcResponder(Deliver deliver);

    auto respond(const rpc::Json& result, std::vector<std::byte> attachment = {}) -> void;
    auto fail(int code, std::string message) -> void;
    auto done() const -> bool;
    auto valid() const -> bool { return state != nullptr; }

private:
    struct State {
        Deliver deliver;
        bool done = false;
    };
    std::shared_ptr<State> state;
};
