#include "uicommands.h"

#include "viewui.h"

#include <nlohmann/json.hpp>

#include <expected>
#include <format>
#include <string>

namespace {

using rpc::Json;

auto parseWindow(const std::string& args) -> std::expected<Json, std::string> {
    auto space = args.find(' ');
    bool open = space == std::string::npos || args.substr(space + 1) != "off";
    return Json{{"name", args.substr(0, space)}, {"open", open}};
}

} // namespace

auto registerUiCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records, ViewUI& ui) -> void {
    registry.add(
        {
            .name = "view.window",
            .summary = "Open or close a window.",
            .params = {
                requiredField("name", RpcType::String, "a window: memory, capture, framedebugger, gpuscene, counters, shaders, culling, assets, lod, streaming, camera, renderdebug, framegraph, performance, config"),
                optionalField("open", RpcType::Bool, "default true"),
            },
            .result = "{}",
        },
        [&ui](const Json& p, RpcResponder responder) {
            auto name = p["name"].get<std::string>();
            if (!ui.setIntrospectionWindow(name, p.value("open", true))) {
                responder.fail(rpc::appError, std::format("unknown window '{}'", name));
                return;
            }
            responder.respond(Json::object());
        });
    verbs.add("window", "view.window", parseWindow);

    records.add("windows", "the UI's windows by name and whether each is open (view.window opens and closes them)", [&ui](RpcResponder responder) {
        auto windows = Json::object();
        for (const auto* name : ViewUI::windowNames) {
            windows[name] = *ui.windowFlag(name);
        }
        responder.respond({{"windows", windows}});
    });
}
