#include "viewcommands.h"

#include "cvar.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <sstream>
#include <string_view>

// The cvars' commands (src/cvar/README.md): cvar.get, set and reset, the cvars record, and the console commands that set values,
// `cvar` and the older short forms.

namespace {

using rpc::Json;

// `cvar <name> <value>`.
auto parseCvar(const std::string& args) -> std::expected<Json, std::string> {
    auto space = args.find(' ');
    if (space == std::string::npos) {
        return std::unexpected(std::format("expected '<name> <value>', got '{}'", args));
    }
    return Json{{"name", args.substr(0, space)}, {"value", args.substr(space + 1)}};
}

// `cvar-reset [<name or prefix>]`.
auto parseCvarReset(const std::string& args) -> std::expected<Json, std::string> {
    return Json{{"name", args}};
}

auto parseViewMode(const std::string& args) -> std::expected<Json, std::string> {
    return cvarValues({{"render.view.mode", args}});
}

auto parseDebugView(const std::string& args) -> std::expected<Json, std::string> {
    return cvarValues({{"render.view.debug", args}});
}

// `overlay name=on|off,…`: the viewport's overlays by their short names.
auto parseOverlay(const std::string& args) -> std::expected<Json, std::string> {
    static constexpr std::array<std::pair<std::string_view, const char*>, 10> overlays = {{
        {"grid", "tools.overlay.grid"},
        {"origin", "tools.overlay.origin"},
        {"gizmo", "tools.overlay.gizmos"},
        {"gizmos", "tools.overlay.gizmos"},
        {"aabbs", "tools.overlay.aabbs"},
        {"lightgizmos", "tools.overlay.lightGizmos"},
        {"cascadefrusta", "tools.overlay.cascadeFrusta"},
        {"buffer", "render.overlay.buffer"},
        {"shadow", "render.overlay.shadow"},
        {"aa", "render.antialiasing"},
    }};
    Json values = Json::object();
    for (const auto& [name, state] : ConsoleCommands::items(args)) {
        auto found = std::ranges::find(overlays, std::string_view(name), &std::pair<std::string_view, const char*>::first);
        if (found == overlays.end() || state.empty()) {
            return std::unexpected(std::format("expected name=on|off for grid, origin, gizmo, aabbs, lightgizmos, buffer, shadow, aa or cascadefrusta, got '{}'", name));
        }
        values[found->second] = state == "on" ? "true" : "false";
    }
    return Json{{"values", values}};
}

auto parseCull(const std::string& args) -> std::expected<Json, std::string> {
    if (args == "on" || args == "off") {
        return cvarValues({{"render.culling.enabled", args}});
    }
    if (args == "freeze" || args == "unfreeze") {
        return cvarValues({{"render.culling.frozen", args == "freeze" ? "true" : "false"}});
    }
    if (args == "show" || args == "hide") {
        return cvarValues({{"tools.overlay.culled", args == "show" ? "true" : "false"}});
    }
    if (args.starts_with("view ")) {
        return cvarValues({{"tools.overlay.culledView", args.substr(5)}});
    }
    return std::unexpected(std::format("unknown argument '{}'", args));
}

auto parseSampler(const std::string& args) -> std::expected<Json, std::string> {
    Json values = Json::object();
    for (const auto& [key, value] : ConsoleCommands::items(args)) {
        if (key == "aniso") {
            values["render.sampler.anisotropy"] = value;
        } else if (key == "bias") {
            values["render.sampler.lodBias"] = value;
        } else if (key == "minlod") {
            values["render.sampler.minLod"] = value;
        } else if (key == "mip") {
            values["render.sampler.nearestMip"] = value == "nearest" ? "true" : "false";
        } else {
            return std::unexpected(std::format("unknown key '{}'", key));
        }
    }
    return Json{{"values", values}};
}

auto parseShadow(const std::string& args) -> std::expected<Json, std::string> {
    Json values = Json::object();
    for (const auto& [key, value] : ConsoleCommands::items(args)) {
        if (key == "cascades") {
            values["render.shadow.cascades"] = value;
        } else if (key == "tile") {
            values["render.shadow.tile"] = value;
        } else if (key == "lambda") {
            values["render.shadow.splitLambda"] = value;
        } else if (key == "pcf") {
            values["render.shadow.pcf"] = value;
        } else if (key == "distance") {
            values["render.shadow.distance"] = value;
        } else {
            return std::unexpected(std::format("unknown key '{}'", key));
        }
    }
    return Json{{"values", values}};
}

// "sky=r,g,b ground=r,g,b", either or both, or "reset".
auto parseAmbient(const std::string& args) -> std::expected<Json, std::string> {
    Json params = {{"values", Json::object()}};
    std::istringstream in(args);
    std::string item;
    while (in >> item) {
        if (item == "reset") {
            params["reset"] = "render.ambient";
            continue;
        }
        auto equals = item.find('=');
        auto key = item.substr(0, equals);
        if (equals == std::string::npos || (key != "sky" && key != "ground")) {
            return std::unexpected(std::format("expected 'sky=r,g,b', 'ground=r,g,b' or 'reset', got '{}'", item));
        }
        params["values"]["render.ambient." + key] = item.substr(equals + 1);
    }
    return params;
}

auto parseGpuScene(const std::string& args) -> std::expected<Json, std::string> {
    Json values = Json::object();
    for (const auto& [key, value] : ConsoleCommands::items(args)) {
        if (key == "budget") {
            values["render.upload.budgetMiB"] = value;
        } else if (key == "limit") {
            values["render.geometry.limitMiB"] = value;
        } else {
            return std::unexpected(std::format("unknown key '{}'", key));
        }
    }
    return Json{{"values", values}};
}

auto parsePrepass(const std::string& args) -> std::expected<Json, std::string> {
    return cvarValues({{"render.prepass", args}});
}

auto parseOcclusion(const std::string& args) -> std::expected<Json, std::string> {
    return cvarValues({{"render.culling.occlusion", args}});
}

auto parseHzbLevel(const std::string& args) -> std::expected<Json, std::string> {
    return cvarValues({{"render.view.hzblevel", args}});
}

auto parseAntiAliasing(const std::string& args) -> std::expected<Json, std::string> {
    return cvarValues({{"render.antialiasing", args}});
}

// A cvar as JSON: typed (a bool, a number, a choice's name, [x, y, z]).
auto cvarValueJson(const cvar::Value& value) -> Json {
    switch (value.type()) {
        case cvar::Type::Bool:
            return static_cast<const cvar::Bool&>(value).get();
        case cvar::Type::Int:
            return static_cast<const cvar::Int&>(value).get();
        case cvar::Type::Float:
            return static_cast<const cvar::Float&>(value).get();
        case cvar::Type::Enum:
            return static_cast<const cvar::Enum&>(value).choice();
        case cvar::Type::Vec3: {
            // At float precision: 0.2, not 0.20000000298 (a float widened to double).
            auto v = static_cast<const cvar::Vec3&>(value).get();
            auto number = [](float f) {
                return std::stod(std::format("{}", f));
            };
            return Json::array({number(v[0]), number(v[1]), number(v[2])});
        }
        case cvar::Type::String:
            return static_cast<const cvar::String&>(value).get();
    }
    return nullptr;
}

auto cvarEntryJson(const cvar::Value& value) -> Json {
    Json out = {
        {"name", value.name()},
        {"type", cvar::typeName(value.type())},
        {"value", cvarValueJson(value)},
        {"default", value.defaultText()},
        {"isDefault", value.isDefault()},
        {"description", value.description()},
    };
    const cvar::Range* range = nullptr;
    if (value.type() == cvar::Type::Int) {
        range = &static_cast<const cvar::Int&>(value).range();
    } else if (value.type() == cvar::Type::Float) {
        range = &static_cast<const cvar::Float&>(value).range();
    } else if (value.type() == cvar::Type::Enum) {
        out["choices"] = static_cast<const cvar::Enum&>(value).choices();
    }
    if (range != nullptr && range->min) {
        out["min"] = *range->min;
    }
    if (range != nullptr && range->max) {
        out["max"] = *range->max;
    }
    return out;
}

// The values a name names: the one value by that name, else those under it as a prefix (every value for an empty name).
auto cvarNamed(const std::string& name) -> std::vector<cvar::Value*> {
    if (auto* value = cvar::find(name)) {
        return {value};
    }
    return cvar::under(name);
}

auto cvarRecord(const std::string& name) -> Json {
    auto values = Json::array();
    for (const auto* value : cvarNamed(name)) {
        values.push_back(cvarEntryJson(*value));
    }
    return {{"values", values}};
}

// A JSON value as the text a cvar parses: true/false, a number, a name, "x,y,z".
auto cvarText(const Json& value) -> std::string {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_array()) {
        std::string out;
        for (const auto& item : value) {
            out += (out.empty() ? "" : ",") + cvarText(item);
        }
        return out;
    }
    return value.dump();
}

} // namespace

// The console commands that set cvars (`view lit`, `prepass on`, …): their arguments as cvar.set's {values}.
auto cvarValues(std::initializer_list<std::pair<const char*, std::string>> values) -> Json {
    Json out = Json::object();
    for (const auto& [name, value] : values) {
        out[name] = value;
    }
    return Json{{"values", out}};
}

auto registerCvarCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records) -> void {
    registry.add(
        {
            .name = "cvar.get",
            .summary = "Cvars (src/cvar/README.md): the one by a name, or every one under a prefix (all without one); each with its type, value, default, description and range or choices.",
            .params = {optionalField("name", RpcType::String, "a value's name, or a prefix such as render.shadow")},
            .result = "{values: [{name, type, value, default, isDefault, description, min?, max?, choices?}]}",
        },
        [](const Json& p, RpcResponder responder) { responder.respond(cvarRecord(p.value("name", std::string()))); });
    registry.add(
        {
            .name = "cvar.set",
            .summary = "Set cvars: one by name and value, or several as {values: {name: value}}; reset (a name or prefix) is put back to its defaults first. Values are checked (range, choices); nothing is set when one fails.",
            .params = {optionalField("name", RpcType::String, "a value's name"),
                       {.name = "value", .type = RpcType::Any, .required = false, .description = "its value: a bool, number, choice's name, [x, y, z] or their text"},
                       optionalField("values", RpcType::Object, "name to value, several at once"),
                       optionalField("reset", RpcType::String, "a name or prefix put back to its defaults first")},
            .result = "{values: [the values set, as cvar.get]}",
        },
        [](const Json& p, RpcResponder responder) {
            std::vector<std::pair<cvar::Value*, std::string>> sets;
            auto add = [&](const std::string& name, const Json& value) -> bool {
                auto* found = cvar::find(name);
                if (found == nullptr) {
                    responder.fail(rpc::invalidParams, std::format("no cvar '{}'", name));
                    return false;
                }
                sets.emplace_back(found, cvarText(value));
                return true;
            };
            if (p.contains("name") && !add(p["name"].get<std::string>(), p.contains("value") ? p["value"] : Json(""))) {
                return;
            }
            if (p.contains("values")) {
                for (const auto& [name, value] : p["values"].items()) {
                    if (!add(name, value)) {
                        return;
                    }
                }
            }
            // Checked before any is set: each value set and put back, so a failing one leaves every value as it was.
            for (auto& [value, text] : sets) {
                auto before = value->text();
                if (auto result = value->setText(text); !result) {
                    responder.fail(rpc::invalidParams, result.error());
                    return;
                }
                (void) value->setText(before);
            }
            if (p.contains("reset")) {
                for (auto* value : cvarNamed(p["reset"].get<std::string>())) {
                    value->reset();
                }
            }
            auto out = Json::array();
            for (auto& [value, text] : sets) {
                (void) value->setText(text);
                out.push_back(cvarEntryJson(*value));
            }
            responder.respond({{"values", out}});
        });
    registry.add(
        {
            .name = "cvar.reset",
            .summary = "Put cvars back to their defaults: the one by a name, or every one under a prefix (all without one).",
            .params = {optionalField("name", RpcType::String, "a value's name or a prefix")},
            .result = "{values: [the values reset, as cvar.get]}",
        },
        [](const Json& p, RpcResponder responder) {
            auto values = cvarNamed(p.value("name", std::string()));
            auto out = Json::array();
            for (auto* value : values) {
                value->reset();
                out.push_back(cvarEntryJson(*value));
            }
            responder.respond({{"values", out}});
        });

    console.add("cvar", "cvar.set", parseCvar);
    console.add("cvar-reset", "cvar.reset", parseCvarReset);
    console.add("view", "cvar.set", parseViewMode);
    console.add("debugview", "cvar.set", parseDebugView);
    console.add("overlay", "cvar.set", parseOverlay);
    console.add("cull", "cvar.set", parseCull);
    console.add("sampler", "cvar.set", parseSampler);
    console.add("shadow", "cvar.set", parseShadow);
    console.add("prepass", "cvar.set", parsePrepass);
    console.add("occlusion", "cvar.set", parseOcclusion);
    console.add("hzb-level", "cvar.set", parseHzbLevel);
    console.add("aa", "cvar.set", parseAntiAliasing);
    console.add("ambient", "cvar.set", parseAmbient);
    console.add("gpuscene", "cvar.set", parseGpuScene);

    records.add("cvars", "the cvars: every value with its type, value, default, description and range or choices", [](RpcResponder responder) {
        responder.respond(cvarRecord(""));
    });
}
