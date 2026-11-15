#include "viewcommands.h"

#include "log.h"
#include "profile.h"
#include "rpcendpoint.h"
#include "sessionscript.h"

#include <nlohmann/json.hpp>

#include <expected>
#include <format>
#include <fstream>
#include <sstream>

namespace {

using rpc::Json;

auto parseRecord(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string name;
    std::string path;
    std::string extra;
    if (!(in >> name >> path) || (in >> extra)) {
        return std::unexpected(std::format("expected '<record> <file>', such as 'render records.jsonl', got '{}'", args));
    }
    return Json{{"name", name}, {"path", path}};
}

} // namespace

auto registerViewCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records, ViewContext& view) -> void {
    registerCameraCommands(registry, console, view);
    registerSceneCommands(registry, console, records, view);
    registerLodCommands(registry, console, records, view);
    registerCvarCommands(registry, console, records);
    registerDisplayCommands(registry, console, records, view);
    registerProfileCommands(registry, console, records);

    // A record taken at a known frame, for scripted runs: the view samples it, so the frame is exact, and appends it to
    // a file, so nothing has to be connected at that frame to receive it.
    registry.add(
        {
            .name = "introspect.record",
            .summary = "Append a record to a file as one JSON line: {record, requested_frame, frame, ts_ns, value}.",
            .params = {
                requiredField("name", RpcType::String, "record name, from introspect.list"),
                requiredField("path", RpcType::String, "the file the line is appended to"),
            },
            .result = "{path, frame} once the line is written",
        },
        [&view, &records](const Json& p, RpcResponder responder) {
            auto name = p["name"].get<std::string>();
            if (!records.contains(name)) {
                responder.fail(rpc::invalidParams, std::format("no record '{}'", name));
                return;
            }
            auto path = p["path"].get<std::string>();
            auto requested = view.frameCounter;
            RpcResponder write([&view, name, path, requested, responder](const RpcReply& reply) mutable {
                if (!reply.ok) {
                    responder.fail(reply.code, reply.message);
                    return;
                }
                nlohmann::ordered_json line;
                line["record"] = name;
                line["requested_frame"] = requested;
                line["frame"] = view.frameCounter;
                line["ts_ns"] = logging::now();
                line["value"] = *reply.result;
                std::ofstream out(path, std::ios::app);
                out << line.dump() << '\n';
                if (!out) {
                    responder.fail(rpc::appError, std::format("cannot write {}", path));
                    return;
                }
                responder.respond({{"path", path}, {"frame", view.frameCounter}});
            });
            records.get(name, write);
        });
    console.add("record", "introspect.record", parseRecord);

    // A console line as a method, for a caller that has a line of text and not a method's parameters: the watch page's console.
    registry.add(
        {
            .name = "console.run",
            .summary = "Run a console command as a script line does: its name, then its text.",
            .params = {requiredField("line", RpcType::String, "the console command and its arguments, such as 'camera-frame scene' or 'cvar render.shadow off'")},
            .result = "what the console command's method answers",
        },
        [&registry, &console](const Json& p, RpcResponder responder) {
            auto line = p["line"].get<std::string>();
            auto start = line.find_first_not_of(" \t");
            if (start == std::string::npos) {
                responder.fail(rpc::invalidParams, "an empty line");
                return;
            }
            auto nameEnd = line.find(' ', start);
            auto name = line.substr(start, nameEnd == std::string::npos ? std::string::npos : nameEnd - start);
            std::string args;
            if (nameEnd != std::string::npos) {
                auto argsStart = line.find_first_not_of(" \t", nameEnd);
                if (argsStart != std::string::npos) {
                    args = line.substr(argsStart);
                    args.erase(args.find_last_not_of(" \t") + 1);
                }
            }
            console.run(registry, name, args, std::move(responder));
        });
}

auto runFrameCommands(ViewContext& view, SessionScript& session, const RpcRegistry& registry, const ConsoleCommands& console, RpcEndpoint& endpoint) -> void {
    auto& script = view.script;
    script.held = (script.waitForReady && !view.readiness.everReady) || !view.readyWaiters.empty() || view.flight.flying();
    std::vector<SessionCommand> due;
    if (!script.held) {
        script.frame++;
        session.takeDue(script.frame, due);
    }
    {
        PROFILE_ZONE("Commands", Frame);
        for (const auto& command : due) {
            console.run(registry, command.name, command.args);
        }
        endpoint.drain();
    }
    if (script.lastFrame > 0 && script.frame > script.lastFrame) {
        view.quit = true;
    }
}

auto answerReadyWaiters(ViewContext& view) -> void {
    if (!view.readiness.ready) {
        return;
    }
    for (auto& waiter : view.readyWaiters) {
        waiter.responder.respond(waiter.result);
    }
    view.readyWaiters.clear();
}
