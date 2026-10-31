#pragma once

#include <array>

// The Configuration window: every configuration value (src/config/README.md), grouped by its name's first two parts, each with an editor
// for its type, its description and its default; values changed from their default marked, and reset per value and per group.
struct ConfigWindowState {
    std::array<char, 128> filter = {};
    bool onlyChanged = false;
};

auto drawConfigWindow(bool& show, ConfigWindowState& state) -> void;
