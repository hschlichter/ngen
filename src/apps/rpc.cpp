// ngen-rpc: command-line client for the RPC endpoints of running ngen processes.
//
//   ngen-rpc list                                 every live endpoint (kind, pid, port, label)
//   ngen-rpc describe <target>                    the target's methods with parameter schemas
//   ngen-rpc call <target> <method> [params]      call a method; params is a JSON object
//
// A target is a kind ("view"), a kind and pid ("view:12345"), or a pid. Endpoints are found
// through the discovery files in .ngen-discovery/ in the working directory (src/rpc/README.md).
//
// Output is JSON on stdout. Exit codes: 0 success, 1 the call returned an error, 2 no such
// endpoint or it can't be reached, 3 bad usage.

#include "rpcclient.h"
#include "rpcdiscovery.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <expected>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int exitOk = 0;
constexpr int exitCallFailed = 1;
constexpr int exitNoEndpoint = 2;
constexpr int exitUsage = 3;

auto usage() -> int {
    std::println(stderr, "usage: ngen-rpc list");
    std::println(stderr, "       ngen-rpc describe <target>");
    std::println(stderr, "       ngen-rpc call <target> <method> [params-json]");
    std::println(stderr, "target: <kind>, <kind>:<pid> or <pid>");
    return exitUsage;
}

auto parsePid(std::string_view text) -> int {
    int value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc() && ptr == text.data() + text.size() ? value : 0;
}

// The one endpoint a target names, or an error listing the candidates.
auto resolveTarget(std::string_view target, const std::vector<RpcEndpointInfo>& endpoints) -> std::expected<RpcEndpointInfo, std::string> {
    std::string kind(target);
    int pid = parsePid(target);
    if (auto colon = target.find(':'); colon != std::string_view::npos) {
        kind = std::string(target.substr(0, colon));
        pid = parsePid(target.substr(colon + 1));
    } else if (pid != 0) {
        kind.clear();
    }
    std::vector<RpcEndpointInfo> matches;
    for (const auto& e : endpoints) {
        bool kindMatches = kind.empty() || e.kind == kind;
        bool pidMatches = pid == 0 || e.pid == pid;
        if (kindMatches && pidMatches) {
            matches.push_back(e);
        }
    }
    if (matches.size() == 1) {
        return matches.front();
    }
    if (matches.empty()) {
        return std::unexpected(std::format("no running endpoint matches '{}'", target));
    }
    std::string list;
    for (const auto& e : matches) {
        list += std::format("\n  {}:{}  {}", e.kind, e.pid, e.label);
    }
    return std::unexpected(std::format("'{}' matches {} endpoints; pick one by pid:{}", target, matches.size(), list));
}

auto commandList(const std::vector<RpcEndpointInfo>& endpoints) -> int {
    auto list = nlohmann::json::array();
    for (const auto& e : endpoints) {
        list.push_back({{"kind", e.kind}, {"pid", e.pid}, {"port", e.port}, {"label", e.label}, {"protocol", e.protocol}});
    }
    std::println("{}", list.dump(2));
    return exitOk;
}

auto commandCall(const RpcEndpointInfo& endpoint, std::string_view method, const nlohmann::json& params) -> int {
    RpcClient client;
    if (auto connected = client.connect(endpoint.port); !connected) {
        std::println(stderr, "ngen-rpc: {}:{}: {}", endpoint.kind, endpoint.pid, connected.error());
        return exitNoEndpoint;
    }
    auto response = client.call(method, params);
    if (!response) {
        std::println(stderr, "ngen-rpc: {}", response.error());
        return exitNoEndpoint;
    }
    const auto& message = response->message;
    if (message.contains("error")) {
        std::println("{}", message["error"].dump(2));
        return exitCallFailed;
    }
    std::println("{}", message["result"].dump(2));
    return exitOk;
}

} // namespace

auto main(int argc, char** argv) -> int {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        return usage();
    }
    auto endpoints = listRpcEndpoints();
    const auto& command = args[0];
    if (command == "list") {
        return commandList(endpoints);
    }
    if (command != "describe" && command != "call") {
        return usage();
    }
    if (args.size() < 2 || (command == "call" && args.size() < 3)) {
        return usage();
    }
    auto endpoint = resolveTarget(args[1], endpoints);
    if (!endpoint) {
        std::println(stderr, "ngen-rpc: {}", endpoint.error());
        return exitNoEndpoint;
    }
    if (command == "describe") {
        return commandCall(*endpoint, "rpc.describe", nlohmann::json::object());
    }
    auto params = nlohmann::json::object();
    if (args.size() >= 4) {
        params = nlohmann::json::parse(args[3], nullptr, false);
        if (params.is_discarded() || !params.is_object()) {
            std::println(stderr, "ngen-rpc: params must be a JSON object, got '{}'", args[3]);
            return exitUsage;
        }
    }
    return commandCall(*endpoint, args[2], params);
}
