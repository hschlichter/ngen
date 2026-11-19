#include "consolecommands.h"

#include "log.h"
#include "rpcregistry.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <format>

auto ConsoleCommands::add(std::string name, std::string method, Parser parse) -> void {
    auto [it, inserted] = commands.try_emplace(std::move(name), Command{.method = std::move(method), .parse = parse});
    assert(inserted && "a console command added twice");
    (void) it;
    (void) inserted;
}

auto ConsoleCommands::run(const RpcRegistry& registry, const std::string& name, const std::string& args) const -> void {
    auto found = commands.find(name);
    if (found == commands.end()) {
        LOG_ERROR("Engine", "ScriptCommandFailed", name).text(std::format("unknown console command '{}'", name));
        return;
    }
    auto params = found->second.parse(args);
    if (!params) {
        LOG_ERROR("Engine", "ScriptCommandFailed", name).text(std::format("{}: {}", name, params.error()));
        return;
    }
    RpcResponder responder([name](const RpcReply& reply) {
        if (!reply.ok) {
            LOG_ERROR("Engine", "ScriptCommandFailed", name).text(std::format("{}: {}", name, reply.message));
        }
    });
    registry.invoke(found->second.method, *params, responder);
}

auto ConsoleCommands::run(const RpcRegistry& registry, const std::string& name, const std::string& args, RpcResponder responder) const -> void {
    auto found = commands.find(name);
    if (found == commands.end()) {
        responder.fail(rpc::appError, std::format("unknown console command '{}'", name));
        return;
    }
    auto params = found->second.parse(args);
    if (!params) {
        responder.fail(rpc::invalidParams, std::format("{}: {}", name, params.error()));
        return;
    }
    registry.invoke(found->second.method, *params, responder);
}

auto ConsoleCommands::items(const std::string& args) -> std::vector<std::pair<std::string, std::string>> {
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

auto ConsoleCommands::none(const std::string&) -> std::expected<rpc::Json, std::string> {
    return rpc::Json::object();
}
