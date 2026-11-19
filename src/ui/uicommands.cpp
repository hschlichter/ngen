#include "uicommands.h"

#include "viewui.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cfloat>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace {

using rpc::Json;

auto parseWindow(const std::string& args) -> std::expected<Json, std::string> {
    auto space = args.find(' ');
    bool open = space == std::string::npos || args.substr(space + 1) != "off";
    return Json{{"name", args.substr(0, space)}, {"open", open}};
}

// A key by the name a browser gives it (KeyboardEvent.key): the keys the UI's windows use, not every key there is.
auto keyNamed(std::string_view name) -> ImGuiKey {
    static constexpr std::pair<std::string_view, ImGuiKey> named[] = {
        {"Enter", ImGuiKey_Enter},
        {"Escape", ImGuiKey_Escape},
        {"Backspace", ImGuiKey_Backspace},
        {"Delete", ImGuiKey_Delete},
        {"Tab", ImGuiKey_Tab},
        {"ArrowLeft", ImGuiKey_LeftArrow},
        {"ArrowRight", ImGuiKey_RightArrow},
        {"ArrowUp", ImGuiKey_UpArrow},
        {"ArrowDown", ImGuiKey_DownArrow},
        {"Home", ImGuiKey_Home},
        {"End", ImGuiKey_End},
        {"PageUp", ImGuiKey_PageUp},
        {"PageDown", ImGuiKey_PageDown},
        {" ", ImGuiKey_Space},
        {"Control", ImGuiMod_Ctrl},
        {"Shift", ImGuiMod_Shift},
        {"Alt", ImGuiMod_Alt},
    };
    for (const auto& [text, key] : named) {
        if (text == name) {
            return key;
        }
    }
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') {
        return (ImGuiKey) (ImGuiKey_A + (name[0] - 'a'));
    }
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
        return (ImGuiKey) (ImGuiKey_A + (name[0] - 'A'));
    }
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
        return (ImGuiKey) (ImGuiKey_0 + (name[0] - '0'));
    }
    return ImGuiKey_None;
}

auto wantsJson(const ViewUI::InputWants& wants) -> Json {
    return {{"wantMouse", wants.mouse}, {"wantKeyboard", wants.keyboard}, {"wantText", wants.text}, {"popup", wants.popup}};
}

} // namespace

auto registerUiCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records, ViewUI& ui) -> void {
    registry.add(
        {
            .name = "view.window",
            .summary = "Open or close a window.",
            .params = {
                requiredField("name", RpcType::String, "a window: memory, capture, framedebugger, gpuscene, counters, shaders, remoteview, culling, assets, lod, streaming, camera, renderdebug, framegraph, performance, cvars"),
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
    console.add("window", "view.window", parseWindow);

    // The UI's input from somewhere other than the window: the watch page, whose pointer is over the video and not over this window.
    // The events go into Dear ImGui's queue as the window's do, so every window, menu and popup takes them the same.
    registry.add(
        {
            .name = "ui.input",
            .summary = "Give the UI input as the window does: a pointer position in framebuffer pixels, a button going down or up, wheel "
                       "steps, typed text, a key. Answers once the frame that took it is drawn, with what the UI wants of the input then: "
                       "the caller sends the pointer on to the UI while wantMouse, and to the camera otherwise.",
            .params =
                {
                    optionalField("x", RpcType::Float, "pointer column, in framebuffer pixels (as view.pick's)"),
                    optionalField("y", RpcType::Float, "pointer row"),
                    optionalField("leave", RpcType::Bool, "the pointer left: nothing is hovered"),
                    optionalField("button", RpcType::Int, "with down: 0 left, 1 right, 2 middle"),
                    optionalField("down", RpcType::Bool, "the button, or the key, went down (true) or came up (false)"),
                    optionalField("wheel", RpcType::Float, "wheel steps, positive away from the user"),
                    optionalField("text", RpcType::String, "typed characters, UTF-8"),
                    optionalField("key", RpcType::String, "a key by its browser name: Enter, Escape, Backspace, Delete, Tab, ArrowLeft, Home, a letter, a digit, Control, Shift, Alt"),
                },
            .result = "{wantMouse, wantKeyboard, wantText, popup} after the next UI frame",
        },
        [&ui](const Json& p, RpcResponder responder) {
            auto& io = ImGui::GetIO();
            if (p.value("leave", false)) {
                io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            } else if (p.contains("x") && p.contains("y")) {
                // ImGui's pointer is in the window's points; a framebuffer pixel is that times the display's scale.
                float scaleX = io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
                float scaleY = io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;
                io.AddMousePosEvent(p["x"].get<float>() / scaleX, p["y"].get<float>() / scaleY);
            }
            if (p.contains("key")) {
                auto key = keyNamed(p["key"].get<std::string>());
                if (key == ImGuiKey_None) {
                    responder.fail(rpc::invalidParams, std::format("key: '{}' is not a key the UI takes", p["key"].get<std::string>()));
                    return;
                }
                io.AddKeyEvent(key, p.value("down", true));
            } else if (p.contains("button") || p.contains("down")) {
                int button = p.value("button", 0);
                if (button < 0 || button > 2) {
                    responder.fail(rpc::invalidParams, "button: 0 left, 1 right, 2 middle");
                    return;
                }
                io.AddMouseButtonEvent(button, p.value("down", true));
            }
            if (p.contains("wheel")) {
                io.AddMouseWheelEvent(0.0f, p["wheel"].get<float>());
            }
            if (p.contains("text")) {
                io.AddInputCharactersUTF8(p["text"].get<std::string>().c_str());
            }
            ui.countRemoteInput();
            ui.addInputWaiter([responder](const ViewUI::InputWants& wants) mutable {
                responder.respond(wantsJson(wants));
            });
        });

    records.add("ui", "the UI's input: whether it wants the mouse, the keys or text now, whether a menu or popup is open, and how many ui.input calls it has taken", [&ui](RpcResponder responder) {
        auto out = wantsJson(ui.inputWants());
        out["remoteInputs"] = ui.remoteInputCount();
        responder.respond(out);
    });

    records.add("windows", "the UI's windows by name and whether each is open (view.window opens and closes them)", [&ui](RpcResponder responder) {
        auto windows = Json::object();
        for (const auto* name : ViewUI::windowNames) {
            windows[name] = *ui.windowFlag(name);
        }
        responder.respond({{"windows", windows}});
    });
}
