#include "viewcommands.h"

#include "profilecommands.h"
#include "trace.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <expected>
#include <format>
#include <sstream>

// The profiler's commands: its history written as a Chrome trace, and its records (src/profile/rpc, shared with the asset server).

namespace {

using rpc::Json;

// dump-profile <path> [seconds] [guide|detail]
auto parseDump(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string path;
    in >> path;
    if (path.empty()) {
        return std::unexpected("usage: dump-profile <path> [seconds] [guide|detail]");
    }
    Json params = {{"path", path}};
    std::string word;
    while (in >> word) {
        if (word == "guide" || word == "detail") {
            params["level"] = word;
            continue;
        }
        double seconds = 0.0;
        auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), seconds);
        if (error != std::errc() || end != word.data() + word.size()) {
            return std::unexpected(std::format("dump-profile: '{}' is neither seconds nor guide|detail", word));
        }
        params["seconds"] = seconds;
    }
    return params;
}

} // namespace

auto registerProfileCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records) -> void {
    registry.add(
        {
            .name = "introspect.profile",
            .summary = "Write the profiler history as a Chrome trace: the last `seconds` (default all), `level` guide for guide zones only (default detail: everything).",
            .params =
                {
                    requiredField("path", RpcType::String, "trace JSON path"),
                    optionalField("seconds", RpcType::Float, "the last seconds of history"),
                    optionalField("level", RpcType::String, "guide or detail"),
                },
            .result = "{path}",
        },
        [](const Json& p, RpcResponder responder) {
            auto result = profile::dumpProfile(p);
            if (!result) {
                responder.fail(rpc::appError, result.error());
                return;
            }
            auto path = (*result)["path"].get<std::string>();
            TRACE_EVENT("Engine", "ProfileWritten", path).text(std::format("profile trace written: {}", path));
            responder.respond(*result);
        });

    verbs.add("dump-profile", "introspect.profile", parseDump);

    profile::registerProfileRecords(records);
}
