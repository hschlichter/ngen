#include "rpcverbs.h"

#include "rpcregistry.h"
#include "trace.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <format>

auto RpcVerbs::add(std::string verb, std::string method, Parser parse) -> void {
    auto [it, inserted] = verbs.try_emplace(std::move(verb), Verb{.method = std::move(method), .parse = parse});
    assert(inserted && "a script verb added twice");
    (void) it;
    (void) inserted;
}

auto RpcVerbs::run(const RpcRegistry& registry, const std::string& verb, const std::string& args) const -> void {
    auto found = verbs.find(verb);
    if (found == verbs.end()) {
        TRACE_ERROR("Engine", "ScriptCommandFailed", verb).text(std::format("unknown script command '{}'", verb));
        return;
    }
    auto params = found->second.parse(args);
    if (!params) {
        TRACE_ERROR("Engine", "ScriptCommandFailed", verb).text(std::format("{}: {}", verb, params.error()));
        return;
    }
    RpcResponder responder([verb](const RpcReply& reply) {
        if (!reply.ok) {
            TRACE_ERROR("Engine", "ScriptCommandFailed", verb).text(std::format("{}: {}", verb, reply.message));
        }
    });
    registry.invoke(found->second.method, *params, responder);
}

auto RpcVerbs::items(const std::string& args) -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> items;
    size_t start = 0;
    while (start < args.size()) {
        auto comma = args.find(',', start);
        auto item = args.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        auto eq = item.find('=');
        items.emplace_back(item.substr(0, eq), eq == std::string::npos ? std::string{} : item.substr(eq + 1));
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return items;
}

auto RpcVerbs::none(const std::string&) -> std::expected<rpc::Json, std::string> {
    return rpc::Json::object();
}
