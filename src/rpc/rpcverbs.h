#pragma once

#include "rpcprotocol.h"

#include <expected>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

class RpcRegistry;

// Script verbs: a `--script` line's verb and its text turned into an RPC method's parameters, so a script drives the same methods a live
// call does. Each layer adds the verbs of the methods it registers.
class RpcVerbs {
public:
    // Text in, the method's parameters out; an error is the message after "<verb>: ".
    using Parser = std::expected<rpc::Json, std::string> (*)(const std::string& args);

    auto add(std::string verb, std::string method, Parser parse) -> void;
    // Parses a verb's text and calls its method. Failures (an unknown verb, text that doesn't parse, the method's error) are
    // ScriptCommandFailed trace errors, `<verb>: <message>`.
    auto run(const RpcRegistry& registry, const std::string& verb, const std::string& args) const -> void;

    // `key=value,key=value` items, as several verbs take their arguments.
    static auto items(const std::string& args) -> std::vector<std::pair<std::string, std::string>>;
    // The parser of a verb that takes nothing.
    static auto none(const std::string& args) -> std::expected<rpc::Json, std::string>;

private:
    struct Verb {
        std::string method;
        Parser parse = nullptr;
    };
    std::map<std::string, Verb, std::less<>> verbs;
};
