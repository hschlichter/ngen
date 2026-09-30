#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <vector>

// What every packer program shares: the command line a pack job runs it with.
//
//   <packer> --rule <name> --rule-version <n> --asset <id> --source <path> --out <file> --depfile <file>
//            [--param key=value]...
//
// A packer reads --source, writes the packed asset to --out, and writes a Make-format depfile listing every file it
// read to --depfile. It exits nonzero, with a message on stderr, when it fails.
struct PackerArgs {
    std::string rule;
    uint32_t ruleVersion = 0;
    std::string asset;
    std::string source;
    std::string out;
    std::string depfile;
    std::map<std::string, std::string> params;

    auto param(const std::string& key, const std::string& fallback = {}) const -> std::string;
};

auto parsePackerArgs(int argc, char** argv) -> std::expected<PackerArgs, std::string>;

// Reads a whole file; an error message when it can't.
auto readPackerFile(const std::string& path) -> std::expected<std::vector<std::byte>, std::string>;
