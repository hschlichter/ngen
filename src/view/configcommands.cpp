#include "viewcommands.h"

#include "config.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <sstream>
#include <string_view>

// The configuration's commands (src/config/README.md): config.get, set and reset, the config record, and the verbs that set values,
// `config` and the older short forms.

namespace {

using rpc::Json;

// `config <name> <value>`.
auto parseConfig(const std::string& args) -> std::expected<Json, std::string> {
    auto space = args.find(' ');
    if (space == std::string::npos) {
        return std::unexpected(std::format("expected '<name> <value>', got '{}'", args));
    }
    return Json{{"name", args.substr(0, space)}, {"value", args.substr(space + 1)}};
}

// `config-reset [<name or prefix>]`.
auto parseConfigReset(const std::string& args) -> std::expected<Json, std::string> {
    return Json{{"name", args}};
}

auto parseViewMode(const std::string& args) -> std::expected<Json, std::string> {
    return configValues({{"render.view.buffer", args}});
}

auto parseDebugView(const std::string& args) -> std::expected<Json, std::string> {
    return configValues({{"render.view.debug", args}});
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
    for (const auto& [name, state] : RpcVerbs::items(args)) {
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
        return configValues({{"render.culling.enabled", args}});
    }
    if (args == "freeze" || args == "unfreeze") {
        return configValues({{"render.culling.frozen", args == "freeze" ? "true" : "false"}});
    }
    if (args == "show" || args == "hide") {
        return configValues({{"tools.overlay.culled", args == "show" ? "true" : "false"}});
    }
    if (args.starts_with("view ")) {
        return configValues({{"tools.overlay.culledView", args.substr(5)}});
    }
    return std::unexpected(std::format("unknown argument '{}'", args));
}

auto parseSampler(const std::string& args) -> std::expected<Json, std::string> {
    Json values = Json::object();
    for (const auto& [key, value] : RpcVerbs::items(args)) {
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
    for (const auto& [key, value] : RpcVerbs::items(args)) {
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
    for (const auto& [key, value] : RpcVerbs::items(args)) {
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
    return configValues({{"render.prepass", args}});
}

auto parseOcclusion(const std::string& args) -> std::expected<Json, std::string> {
    return configValues({{"render.culling.occlusion", args}});
}

auto parseAntiAliasing(const std::string& args) -> std::expected<Json, std::string> {
    return configValues({{"render.antialiasing", args}});
}

// A configuration value as JSON: typed (a bool, a number, a choice's name, [x, y, z]).
auto configValueJson(const config::Value& value) -> Json {
    switch (value.type()) {
        case config::Type::Bool:
            return static_cast<const config::Bool&>(value).get();
        case config::Type::Int:
            return static_cast<const config::Int&>(value).get();
        case config::Type::Float:
            return static_cast<const config::Float&>(value).get();
        case config::Type::Enum:
            return static_cast<const config::Enum&>(value).choice();
        case config::Type::Vec3: {
            // At float precision: 0.2, not 0.20000000298 (a float widened to double).
            auto v = static_cast<const config::Vec3&>(value).get();
            auto number = [](float f) {
                return std::stod(std::format("{}", f));
            };
            return Json::array({number(v[0]), number(v[1]), number(v[2])});
        }
    }
    return nullptr;
}

auto configEntryJson(const config::Value& value) -> Json {
    Json out = {
        {"name", value.name()},
        {"type", config::typeName(value.type())},
        {"value", configValueJson(value)},
        {"default", value.defaultText()},
        {"isDefault", value.isDefault()},
        {"description", value.description()},
    };
    const config::Range* range = nullptr;
    if (value.type() == config::Type::Int) {
        range = &static_cast<const config::Int&>(value).range();
    } else if (value.type() == config::Type::Float) {
        range = &static_cast<const config::Float&>(value).range();
    } else if (value.type() == config::Type::Enum) {
        out["choices"] = static_cast<const config::Enum&>(value).choices();
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
auto configNamed(const std::string& name) -> std::vector<config::Value*> {
    if (auto* value = config::find(name)) {
        return {value};
    }
    return config::under(name);
}

auto configRecord(const std::string& name) -> Json {
    auto values = Json::array();
    for (const auto* value : configNamed(name)) {
        values.push_back(configEntryJson(*value));
    }
    return {{"values", values}};
}

// A JSON value as the text a configuration value parses: true/false, a number, a name, "x,y,z".
auto configText(const Json& value) -> std::string {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_array()) {
        std::string out;
        for (const auto& item : value) {
            out += (out.empty() ? "" : ",") + configText(item);
        }
        return out;
    }
    return value.dump();
}

} // namespace

// The verbs that set configuration values (`view lit`, `prepass on`, …): their arguments as config.set's {values}.
auto configValues(std::initializer_list<std::pair<const char*, std::string>> values) -> Json {
    Json out = Json::object();
    for (const auto& [name, value] : values) {
        out[name] = value;
    }
    return Json{{"values", out}};
}

auto registerConfigCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records) -> void {
    registry.add(
        {
            .name = "config.get",
            .summary = "Configuration values (src/config/README.md): the one by a name, or every one under a prefix (all without one); each with its type, value, default, description and range or choices.",
            .params = {optionalField("name", RpcType::String, "a value's name, or a prefix such as render.shadow")},
            .result = "{values: [{name, type, value, default, isDefault, description, min?, max?, choices?}]}",
        },
        [](const Json& p, RpcResponder responder) { responder.respond(configRecord(p.value("name", std::string()))); });
    registry.add(
        {
            .name = "config.set",
            .summary = "Set configuration values: one by name and value, or several as {values: {name: value}}; reset (a name or prefix) is put back to its defaults first. Values are checked (range, choices); nothing is set when one fails.",
            .params = {optionalField("name", RpcType::String, "a value's name"),
                       {.name = "value", .type = RpcType::Any, .required = false, .description = "its value: a bool, number, choice's name, [x, y, z] or their text"},
                       optionalField("values", RpcType::Object, "name to value, several at once"),
                       optionalField("reset", RpcType::String, "a name or prefix put back to its defaults first")},
            .result = "{values: [the values set, as config.get]}",
        },
        [](const Json& p, RpcResponder responder) {
            std::vector<std::pair<config::Value*, std::string>> sets;
            auto add = [&](const std::string& name, const Json& value) -> bool {
                auto* found = config::find(name);
                if (found == nullptr) {
                    responder.fail(rpc::invalidParams, std::format("no configuration value '{}'", name));
                    return false;
                }
                sets.emplace_back(found, configText(value));
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
                for (auto* value : configNamed(p["reset"].get<std::string>())) {
                    value->reset();
                }
            }
            auto out = Json::array();
            for (auto& [value, text] : sets) {
                (void) value->setText(text);
                out.push_back(configEntryJson(*value));
            }
            responder.respond({{"values", out}});
        });
    registry.add(
        {
            .name = "config.reset",
            .summary = "Put configuration values back to their defaults: the one by a name, or every one under a prefix (all without one).",
            .params = {optionalField("name", RpcType::String, "a value's name or a prefix")},
            .result = "{values: [the values reset, as config.get]}",
        },
        [](const Json& p, RpcResponder responder) {
            auto values = configNamed(p.value("name", std::string()));
            auto out = Json::array();
            for (auto* value : values) {
                value->reset();
                out.push_back(configEntryJson(*value));
            }
            responder.respond({{"values", out}});
        });

    verbs.add("config", "config.set", parseConfig);
    verbs.add("config-reset", "config.reset", parseConfigReset);
    verbs.add("view", "config.set", parseViewMode);
    verbs.add("debugview", "config.set", parseDebugView);
    verbs.add("overlay", "config.set", parseOverlay);
    verbs.add("cull", "config.set", parseCull);
    verbs.add("sampler", "config.set", parseSampler);
    verbs.add("shadow", "config.set", parseShadow);
    verbs.add("prepass", "config.set", parsePrepass);
    verbs.add("occlusion", "config.set", parseOcclusion);
    verbs.add("aa", "config.set", parseAntiAliasing);
    verbs.add("ambient", "config.set", parseAmbient);
    verbs.add("gpuscene", "config.set", parseGpuScene);

    records.add("config", "the configuration: every value with its type, value, default, description and range or choices", [](RpcResponder responder) {
        responder.respond(configRecord(""));
    });
}
