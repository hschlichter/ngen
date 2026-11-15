#pragma once

#include "cvar.h"

#include <array>
#include <string_view>

// The Cvars window: every cvar (src/cvar/README.md), grouped by its name's first two parts, each with an editor
// for its type, its description and its default; values changed from their default marked, and reset per value and per group.
struct CvarWindowState {
    std::array<char, 128> filter = {};
    bool onlyChanged = false;
};

auto drawCvarWindow(bool& show, CvarWindowState& state) -> void;

// Cvars by name, for windows that show a value another library declares (the scene's purposes, a tool's layers) without
// including it; null when no value has that name (its library isn't linked). The value's own check applies to a set.
auto cvarBool(std::string_view name) -> cvar::Bool*;
auto cvarFloat(std::string_view name) -> cvar::Float*;
// A checkbox, and a menu item, over a Bool by name; drawn disabled when it doesn't exist. Return the value after the frame's click.
auto cvarCheckbox(const char* label, std::string_view name) -> bool;
auto cvarMenuItem(const char* label, const char* shortcut, std::string_view name, bool enabled = true) -> bool;
