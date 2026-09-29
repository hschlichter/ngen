#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>

// JSON-RPC 2.0 over rpcframe.h frames. Every endpoint speaks the same protocol: requests
// and notifications in both directions on one connection, responses matched by id.
namespace rpc {

using Json = nlohmann::json;

inline constexpr int protocolVersion = 1;

// JSON-RPC error codes. Engine failures ("prim not found") use appError and a message.
inline constexpr int parseError = -32700;
inline constexpr int invalidRequest = -32600;
inline constexpr int methodNotFound = -32601;
inline constexpr int invalidParams = -32602;
inline constexpr int internalError = -32603;
inline constexpr int appError = -32000;

auto makeRequest(const Json& id, std::string_view method, const Json& params) -> Json;
auto makeNotification(std::string_view method, const Json& params) -> Json;
auto makeResult(const Json& id, const Json& result) -> Json;
auto makeError(const Json& id, int code, std::string_view message) -> Json;

// What kind of message a parsed frame holds.
enum class MessageKind {
    Request,      // has "method" and "id"
    Notification, // has "method", no "id"
    Response,     // has "result" or "error", and "id"
    Invalid,
};
auto messageKind(const Json& message) -> MessageKind;

} // namespace rpc
