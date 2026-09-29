#include "rpcresponder.h"

#include <nlohmann/json.hpp>

RpcResponder::RpcResponder(Deliver deliver) : state(std::make_shared<State>()) {
    state->deliver = std::move(deliver);
}

auto RpcResponder::respond(const rpc::Json& result, std::vector<std::byte> attachment) -> void {
    if (state == nullptr || state->done) {
        return;
    }
    state->done = true;
    RpcReply reply = {.ok = true, .result = &result, .attachment = std::move(attachment)};
    state->deliver(reply);
}

auto RpcResponder::fail(int code, std::string message) -> void {
    if (state == nullptr || state->done) {
        return;
    }
    state->done = true;
    RpcReply reply = {.ok = false, .code = code, .message = std::move(message)};
    state->deliver(reply);
}

auto RpcResponder::done() const -> bool {
    return state == nullptr || state->done;
}
