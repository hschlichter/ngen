#include "rpcprotocol.h"

#include <nlohmann/json.hpp>

namespace rpc {

auto makeRequest(const Json& id, std::string_view method, const Json& params) -> Json {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

auto makeNotification(std::string_view method, const Json& params) -> Json {
    return Json{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
}

auto makeResult(const Json& id, const Json& result) -> Json {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

auto makeError(const Json& id, int code, std::string_view message) -> Json {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

auto messageKind(const Json& message) -> MessageKind {
    if (!message.is_object()) {
        return MessageKind::Invalid;
    }
    bool hasMethod = message.contains("method") && message["method"].is_string();
    bool hasId = message.contains("id") && !message["id"].is_null();
    if (hasMethod) {
        return hasId ? MessageKind::Request : MessageKind::Notification;
    }
    if (hasId && (message.contains("result") || message.contains("error"))) {
        return MessageKind::Response;
    }
    return MessageKind::Invalid;
}

} // namespace rpc
