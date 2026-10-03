#include "introspecttarget.h"

#include <charconv>
#include <format>

namespace {

auto parsePid(std::string_view text) -> int {
    int value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        return 0;
    }
    return value;
}

} // namespace

auto processName(const RpcEndpointInfo& endpoint) -> std::string {
    return std::format("{}:{}", endpoint.kind, endpoint.pid);
}

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
        list += std::format("\n  {}  {}", processName(e), e.label);
    }
    return std::unexpected(std::format("'{}' matches {} endpoints; pick one by pid:{}", target, matches.size(), list));
}
