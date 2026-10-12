#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

// What every packer program shares: the command line a pack job runs it with.
//
//   <packer> --rule <name> --rule-version <n> --asset <id> --source <path> --out <file> --depfile <file>
//            [--param key=value]...
//
// A packer reads --source, writes the packed asset to --out, and writes a Make-format depfile listing every file it
// read to --depfile. It exits nonzero, with a message on stderr, when it fails.
//
// A packer may also write secondary outputs: assets the same source defines, with ids "<asset>#<fragment>". It writes
// each to secondaryOutputFile(out, fragment) and lists them all in <out>.outputs, one "<id>\t<file>" line each
// (writeSecondaryOutputs); the asset server records them with the job.
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

// Writes a whole file; an error message when it can't.
auto writePackerFile(const std::string& path, std::span<const std::byte> bytes) -> std::expected<void, std::string>;

// Where a secondary output with this fragment goes, beside the primary output `out`: "<out>#" followed by the fragment with
// every '/' written as '.', so a prim path's outputs never collide with each other (a USD prim name has no '.').
auto secondaryOutputFile(const std::string& out, const std::string& fragment) -> std::string;

// The list of secondary outputs, "<out>.outputs".
auto secondaryOutputsList(const std::string& out) -> std::string;

// Removes the secondary outputs an earlier run of this job wrote, and its list.
auto clearSecondaryOutputs(const std::string& out) -> void;

// Writes <out>.outputs: one "<id>\t<file>" line per secondary output.
auto writeSecondaryOutputs(const std::string& out, const std::vector<std::pair<std::string, std::string>>& outputs) -> std::expected<void, std::string>;

// Reads <out>.outputs; empty when there is none.
auto readSecondaryOutputs(const std::string& out) -> std::vector<std::pair<std::string, std::string>>;
