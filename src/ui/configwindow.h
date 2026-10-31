#pragma once

#include "config.h"

#include <array>
#include <string_view>

// The Configuration window: every configuration value (src/config/README.md), grouped by its name's first two parts, each with an editor
// for its type, its description and its default; values changed from their default marked, and reset per value and per group.
struct ConfigWindowState {
    std::array<char, 128> filter = {};
    bool onlyChanged = false;
};

auto drawConfigWindow(bool& show, ConfigWindowState& state) -> void;

// Configuration values by name, for windows that show a value another library declares (the scene's purposes, a tool's layers) without
// including it; null when no value has that name (its library isn't linked). The value's own check applies to a set.
auto configBool(std::string_view name) -> config::Bool*;
auto configFloat(std::string_view name) -> config::Float*;
// A checkbox, and a menu item, over a Bool by name; drawn disabled when it doesn't exist. Return the value after the frame's click.
auto configCheckbox(const char* label, std::string_view name) -> bool;
auto configMenuItem(const char* label, const char* shortcut, std::string_view name, bool enabled = true) -> bool;
