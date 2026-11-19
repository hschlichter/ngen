#include "remoteviewcommands.h"

#include "profile.h"
#include "remotecontrol.h"
#include "remoteview.h"

#include <nlohmann/json.hpp>

#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>

#include <unistd.h>

namespace {

using Json = rpc::Json;

auto remoteViewJson(const RemoteViewStatus& status, const RemoteControlStatus& control) -> Json {
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
            {"layout", status.report.layout},
            {"pointer", status.report.pointer},
            {"test", status.report.test.empty() ? Json(nullptr) : Json::parse(status.report.test, nullptr, false)},
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
        {"frame", {{"width", status.frameWidth}, {"height", status.frameHeight}}},
        {"control",
         {
             {"enabled", control.enabled},
             {"open", control.open},
             {"calls", control.calls},
             {"failed", control.failed},
             {"tooLarge", control.tooLarge},
             {"dropped", control.dropped},
             {"messageLimit", control.messageLimit},
             {"lastMethod", control.lastMethod},
             {"logSubscribed", control.logSubscribed},
             {"logEvents", control.logEvents},
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

auto registerRemoteViewCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records, RemoteView& remoteView, const RemoteControl& remoteControl) -> void {
    records.add("remoteview",
                "the remote view: whether it listens and where (the URL to open), the watcher, the video as encoded, the frame's size it is scaled from, "
                "remote control (on, the channel open, calls run and failed, the last method), frames captured, replaced, encoded and sent since it was "
                "turned on, the last second's times, and what the watch page last reported (frames decoded, latency, its layout, its self-test); its "
                "settings are the cvars record's remoteview.*",
                [&remoteView, &remoteControl](RpcResponder responder) {
                    responder.respond(remoteViewJson(remoteView.status(), remoteControl.status()));
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

    registry.add(
        {
            .name = "remoteview.screenshot",
            .summary = "Take a screenshot (view.screenshot, into the temporary directory) and give it to the watch page to download: the PNG "
                       "is also served at the URL answered, on the watch page's address, until the next one replaces it.",
            .result = "{path, url, width, height}: the file written, and the watch page's path to fetch it at",
        },
        [&registry, &remoteView](const Json&, RpcResponder responder) {
            RpcResponder taken([&remoteView, responder](const RpcReply& reply) mutable {
                if (!reply.ok) {
                    responder.fail(reply.code, reply.message);
                    return;
                }
                auto path = reply.result->value("path", std::string());
                std::ifstream in(path, std::ios::binary);
                if (!in) {
                    responder.fail(rpc::appError, std::format("cannot read {}", path));
                    return;
                }
                std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                auto name = std::filesystem::path(path).filename().string();
                remoteView.shareFile(name, "image/png", std::move(bytes));
                responder.respond({{"path", path}, {"url", "/file/" + name}, {"width", reply.result->value("width", 0)}, {"height", reply.result->value("height", 0)}});
            });
            // One file in the temporary directory, written over by the next: the watch page downloads it, nothing collects.
            auto path = std::filesystem::temp_directory_path() / std::format("ngen-remoteview-{}.png", (int) getpid());
            registry.invoke("view.screenshot", Json{{"path", path.string()}}, taken);
        });

    console.add("remoteview-keyframe", "remoteview.keyframe", ConsoleCommands::none);
    console.add("remoteview-screenshot", "remoteview.screenshot", ConsoleCommands::none);
    console.add("remoteview-disconnect", "remoteview.disconnect", ConsoleCommands::none);
    console.add("remoteview-grab", "remoteview.grab", parseGrab);
}
