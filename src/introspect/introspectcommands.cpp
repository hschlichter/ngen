#include "introspectcommands.h"

#include "introspecttarget.h"
#include "rpcclient.h"
#include "rpcdiscovery.h"

#include <nlohmann/json.hpp>

#include <expected>
#include <format>
#include <print>
#include <string_view>
#include <vector>

namespace {

constexpr int exitOk = 0;
constexpr int exitCallFailed = 1;
constexpr int exitNoEndpoint = 2;
constexpr int exitUsage = 3;

auto usage() -> int {
    std::println(stderr, "usage: ngen-introspect                                   the window");
    std::println(stderr, "       ngen-introspect list");
    std::println(stderr, "       ngen-introspect get <target> <record>");
    std::println(stderr, "       ngen-introspect describe <target>");
    std::println(stderr, "       ngen-introspect call <target> <method> [params-json]");
    std::println(stderr, "target: <kind>, <kind>:<pid> or <pid>");
    return exitUsage;
}

struct CallFailure {
    int exitCode = exitNoEndpoint;
    std::string message;
};

// One call on a fresh connection: the result, or why there is none. An error the method returned is a
// CallFailure with exitCallFailed and the error as JSON text.
auto callOnce(const RpcEndpointInfo& endpoint, std::string_view method, const nlohmann::json& params) -> std::expected<nlohmann::json, CallFailure> {
    RpcClient client;
    if (auto connected = client.connect(endpoint.port); !connected) {
        return std::unexpected(CallFailure{.exitCode = exitNoEndpoint, .message = std::format("{}: {}", processName(endpoint), connected.error())});
    }
    auto response = client.call(method, params);
    if (!response) {
        return std::unexpected(CallFailure{.exitCode = exitNoEndpoint, .message = std::format("{}: {}", processName(endpoint), response.error())});
    }
    const auto& message = response->message;
    if (message.contains("error")) {
        return std::unexpected(CallFailure{.exitCode = exitCallFailed, .message = message["error"].dump(2)});
    }
    return message["result"];
}

auto printCall(const RpcEndpointInfo& endpoint, std::string_view method, const nlohmann::json& params) -> int {
    auto result = callOnce(endpoint, method, params);
    if (!result) {
        if (result.error().exitCode == exitCallFailed) {
            std::println("{}", result.error().message);
        } else {
            std::println(stderr, "ngen-introspect: {}", result.error().message);
        }
        return result.error().exitCode;
    }
    std::println("{}", result->dump(2));
    return exitOk;
}

auto commandList(const std::vector<RpcEndpointInfo>& endpoints) -> int {
    auto list = nlohmann::json::array();
    for (const auto& e : endpoints) {
        nlohmann::json entry = {
            {"process", processName(e)},
            {"kind", e.kind},
            {"pid", e.pid},
            {"port", e.port},
            {"label", e.label},
            {"protocol", e.protocol},
        };
        auto records = callOnce(e, "introspect.list", nlohmann::json::object());
        if (records) {
            entry["records"] = (*records)["records"];
        } else {
            entry["records"] = nullptr;
            entry["error"] = records.error().message;
        }
        list.push_back(std::move(entry));
    }
    std::println("{}", list.dump(2));
    return exitOk;
}

} // namespace

auto isIntrospectCommand(const std::string& command) -> bool {
    return command == "list" || command == "get" || command == "describe" || command == "call";
}

auto runIntrospectCommand(std::span<const std::string> args) -> int {
    if (args.empty() || !isIntrospectCommand(args[0])) {
        return usage();
    }
    auto endpoints = listRpcEndpoints();
    const auto& command = args[0];
    if (command == "list") {
        return commandList(endpoints);
    }
    if (args.size() < 2) {
        return usage();
    }
    if ((command == "get" || command == "call") && args.size() < 3) {
        return usage();
    }
    auto endpoint = resolveTarget(args[1], endpoints);
    if (!endpoint) {
        std::println(stderr, "ngen-introspect: {}", endpoint.error());
        return exitNoEndpoint;
    }
    if (command == "describe") {
        return printCall(*endpoint, "rpc.describe", nlohmann::json::object());
    }
    if (command == "get") {
        return printCall(*endpoint, "introspect.get", {{"name", args[2]}});
    }
    auto params = nlohmann::json::object();
    if (args.size() >= 4) {
        params = nlohmann::json::parse(args[3], nullptr, false);
        if (params.is_discarded() || !params.is_object()) {
            std::println(stderr, "ngen-introspect: params must be a JSON object, got '{}'", args[3]);
            return exitUsage;
        }
    }
    return printCall(*endpoint, args[2], params);
}
