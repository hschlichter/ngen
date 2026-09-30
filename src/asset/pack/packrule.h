#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// How one type of asset is packed. The project's rules are defined in the root pack.cpp and compiled into
// ngen-asset-server; see src/asset/README.md.
struct PackRule {
    std::string name;
    // File extensions this rule packs, with the dot: ".vert". An extension belongs to one rule.
    std::vector<std::string> extensions;
    // The packer program's file name; it sits next to ngen-asset-server.
    std::string packer;
    // Passed to the packer as --param key=value, in this order.
    std::vector<std::pair<std::string, std::string>> params;
    // Bumped when the packer's output format changes, so every asset of the rule is packed again.
    uint32_t version = 0;
};

// The project's pack rules for one configuration ("debug", "release", …). Defined in the root pack.cpp.
auto packRules(const std::string& config) -> std::vector<PackRule>;
