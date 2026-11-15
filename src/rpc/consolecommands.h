#pragma once

#include "rpcprotocol.h"

#include <expected>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

class RpcRegistry;

// Console commands: a `--script` line's console command and its text turned into an RPC method's parameters, so a script drives the same methods a live
// call does. Each layer adds the console commands of the methods it registers.
class ConsoleCommands {
public:
    // Text in, the method's parameters out; an error is the message after "<command>: ".
    using Parser = std::expected<rpc::Json, std::string> (*)(const std::string& args);

    auto add(std::string name, std::string method, Parser parse) -> void;
    // Parses a console command's text and calls its method. Failures (an unknown console command, text that doesn't parse, the method's error) are
    // ScriptCommandFailed log errors, `<command>: <message>`.
    auto run(const RpcRegistry& registry, const std::string& name, const std::string& args) const -> void;

    // `key=value,key=value` items, as several console commands take their arguments.
    static auto items(const std::string& args) -> std::vector<std::pair<std::string, std::string>>;
    // The parser of a console command that takes nothing.
    static auto none(const std::string& args) -> std::expected<rpc::Json, std::string>;

private:
    struct Command {
        std::string method;
        Parser parse = nullptr;
    };
    std::map<std::string, Command, std::less<>> commands;
};
