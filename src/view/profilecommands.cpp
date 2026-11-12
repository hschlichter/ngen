#include "viewcommands.h"

#include "jobsystem.h"
#include "profilecommands.h"
#include "trace.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <expected>
#include <format>
#include <sstream>

// The profiler's commands: its history written as a Chrome trace, and its records (src/profile/rpc, shared with the asset server); and the
// job system's record, whose workers fill the profiler's worker lanes.

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

    registry.add(
        {
            .name = "introspect.plot",
            .summary = "One profiler plot's samples over the last `seconds` (default the whole history); the plots record lists the plots.",
            .params =
                {
                    requiredField("name", RpcType::String, "the plot's name"),
                    optionalField("seconds", RpcType::Float, "the last seconds of history"),
                },
            .result = "{name, group, format, samples: [{ms, value}]}, ms before now",
        },
        [](const Json& p, RpcResponder responder) {
            auto result = profile::plotSamples(p);
            if (!result) {
                responder.fail(rpc::appError, result.error());
                return;
            }
            responder.respond(*result);
        });

    profile::registerProfileRecords(records);

    records.add("jobs", "the job system: workers, frame and background jobs queued, running and completed, the background job running longest", [](RpcResponder responder) {
        auto stats = JobSystem::stats();
        Json longest = nullptr;
        if (!stats.longestBackground.empty()) {
            longest = {{"name", stats.longestBackground}, {"ms", stats.longestBackgroundMs}};
        }
        responder.respond({
            {"workers", stats.workers},
            {"frame", {{"queued", stats.frameQueued}, {"running", stats.frameRunning}, {"completed", stats.frameCompleted}}},
            {"background", {{"queued", stats.backgroundQueued}, {"running", stats.backgroundRunning}, {"completed", stats.backgroundCompleted}, {"longestRunning", longest}}},
        });
    });
}
