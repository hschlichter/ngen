#include "remoteviewcommands.h"

#include "profile.h"
#include "remoteview.h"

#include <nlohmann/json.hpp>

#include <expected>
#include <format>
#include <string>

namespace {

using Json = rpc::Json;

auto remoteViewJson(const RemoteViewStatus& status) -> Json {
    Json watcher = {
        {"state", videoWatcherStateName(status.watcher)},
        {"address", status.watcherAddress},
        {"connects", status.connects},
        {"disconnects", status.disconnects},
    };
    Json page = nullptr;
    if (status.report.received) {
        Json bars = Json::array();
        for (const auto& bar : status.report.patternBars) {
            bars.push_back(Json::array({bar[0], bar[1], bar[2]}));
        }
        page = {
            {"agent", status.report.agent},
            {"state", status.report.state},
            {"framesDecoded", status.report.framesDecoded},
            {"framesDropped", status.report.framesDropped},
            {"framesPerSecond", status.report.framesPerSecond},
            {"width", status.report.width},
            {"height", status.report.height},
            {"latencyMs", status.report.latencyMs ? Json(*status.report.latencyMs) : Json(nullptr)},
            {"patternBars", bars},
            {"secondsAgo", (double) (profile::now() - status.report.receivedNs) / 1e9},
        };
    }
    return {
        {"enabled", status.enabled},
        {"listening", status.listening},
        {"source", status.source},
        {"address", status.address},
        {"port", status.port},
        {"url", status.url},
        {"urls", status.urls},
        {"watcher", watcher},
        {"video",
         {
             {"encoder", status.encoder},
             {"width", status.width},
             {"height", status.height},
             {"framesPerSecond", status.framesPerSecond},
             {"bitrate", status.bitrate},
         }},
        {"frames",
         {
             {"captured", status.captured},
             {"replaced", status.replaced},
             {"encoded", status.encoded},
             {"sent", status.sent},
             {"sentBytes", status.sentBytes},
             {"keyframes", status.keyframes},
             {"keyframeRequests", status.keyframeRequests},
         }},
        {"ms",
         {
             {"copy", status.copyMs},
             {"convert", status.convertMs},
             {"encode", status.encodeMs},
             {"captureToSend", status.captureToSendMs},
         }},
        {"page", page},
        {"lastError", status.lastError},
    };
}

auto parseGrab(const std::string& args) -> std::expected<Json, std::string> {
    if (args.empty()) {
        return std::unexpected("expected a file, such as 'remoteview-grab /tmp/watched.png'");
    }
    return Json{{"path", args}};
}

} // namespace

auto registerRemoteViewCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records, RemoteView& remoteView) -> void {
    records.add("remoteview",
                "the remote view: whether it listens and where (the URL to open), the watcher, the video as encoded, frames captured, replaced, encoded and "
                "sent since it was turned on, the last second's times, and what the watch page last reported (frames decoded, latency); its settings are "
                "the cvars record's remoteview.*",
                [&remoteView](RpcResponder responder) {
                    responder.respond(remoteViewJson(remoteView.status()));
                });

    registry.add({.name = "remoteview.keyframe", .summary = "Make the next frame sent to the watcher a keyframe.", .result = "{}"},
                 [&remoteView](const Json&, RpcResponder responder) {
                     remoteView.requestKeyframe();
                     responder.respond(Json::object());
                 });
    registry.add({.name = "remoteview.disconnect", .summary = "Drop the watcher. Its watch page connects again by itself after two seconds.", .result = "{}"},
                 [&remoteView](const Json&, RpcResponder responder) {
                     remoteView.disconnect();
                     responder.respond(Json::object());
                 });
    registry.add(
        {
            .name = "remoteview.grab",
            .summary = "Ask the watch page for the frame it is showing and write it as a PNG: what the watcher sees, after decoding.",
            .params = {requiredField("path", RpcType::String, "the PNG file to write")},
            .result = "{path} once the file is written; an error when no watch page answers within five seconds",
        },
        [&remoteView](const Json& p, RpcResponder responder) {
            auto path = p["path"].get<std::string>();
            remoteView.requestGrab(path, [path, responder](std::expected<void, std::string> result) mutable {
                if (!result) {
                    responder.fail(rpc::appError, result.error());
                    return;
                }
                responder.respond(Json{{"path", path}});
            });
        });

    console.add("remoteview-keyframe", "remoteview.keyframe", ConsoleCommands::none);
    console.add("remoteview-disconnect", "remoteview.disconnect", ConsoleCommands::none);
    console.add("remoteview-grab", "remoteview.grab", parseGrab);
}
