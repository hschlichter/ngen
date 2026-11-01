#include "viewcommands.h"

#include "profile.h"
#include "rpcendpoint.h"
#include "sessionscript.h"
#include "trace.h"

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

auto registerViewCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records, ViewContext& view) -> void {
    registerCameraCommands(registry, verbs, view);
    registerSceneCommands(registry, verbs, records, view);
    registerLodCommands(registry, verbs, records, view);
    registerConfigCommands(registry, verbs, records);
    registerDisplayCommands(registry, verbs, records, view);
    registerProfileCommands(registry, verbs, records);

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
                line["ts_ns"] = trace::now();
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
    verbs.add("record", "introspect.record", parseRecord);
}

auto runFrameCommands(ViewContext& view, SessionScript& session, const RpcRegistry& registry, const RpcVerbs& verbs, RpcEndpoint& endpoint) -> void {
    auto& script = view.script;
    script.held = (script.waitForResident && !view.residency.everResident) || !view.residentWaiters.empty() || view.flight.flying();
    std::vector<SessionCommand> due;
    if (!script.held) {
        script.frame++;
        session.takeDue(script.frame, due);
    }
    {
        PROFILE_ZONE("Commands");
        for (const auto& command : due) {
            verbs.run(registry, command.verb, command.args);
        }
        endpoint.drain();
    }
    if (script.lastFrame > 0 && script.frame > script.lastFrame) {
        view.quit = true;
    }
}

auto answerResidentWaiters(ViewContext& view) -> void {
    if (!view.residency.resident) {
        return;
    }
    for (auto& waiter : view.residentWaiters) {
        waiter.responder.respond(waiter.result);
    }
    view.residentWaiters.clear();
}
