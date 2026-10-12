#include "packer.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>

auto PackerArgs::param(const std::string& key, const std::string& fallback) const -> std::string {
    auto it = params.find(key);
    return it != params.end() ? it->second : fallback;
}

auto parsePackerArgs(int argc, char** argv) -> std::expected<PackerArgs, std::string> {
    PackerArgs args;
    for (int i = 1; i < argc; i++) {
        std::string_view flag = argv[i];
        if (i + 1 >= argc) {
            return std::unexpected(std::format("{} needs a value", flag));
        }
        std::string value = argv[++i];
        if (flag == "--rule") {
            args.rule = value;
        } else if (flag == "--rule-version") {
            args.ruleVersion = (uint32_t) std::stoul(value);
        } else if (flag == "--asset") {
            args.asset = value;
        } else if (flag == "--source") {
            args.source = value;
        } else if (flag == "--out") {
            args.out = value;
        } else if (flag == "--depfile") {
            args.depfile = value;
        } else if (flag == "--param") {
            auto eq = value.find('=');
            if (eq == std::string::npos) {
                return std::unexpected(std::format("--param expects key=value, got '{}'", value));
            }
            args.params[value.substr(0, eq)] = value.substr(eq + 1);
        } else {
            return std::unexpected(std::format("unknown argument '{}'", flag));
        }
    }
    if (args.asset.empty() || args.source.empty() || args.out.empty() || args.depfile.empty()) {
        return std::unexpected("--asset, --source, --out and --depfile are required");
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(args.out).parent_path(), ec);
    return args;
}

auto readPackerFile(const std::string& path) -> std::expected<std::vector<std::byte>, std::string> {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::unexpected(std::format("cannot read {}", path));
    }
    auto size = (size_t) in.tellg();
    in.seekg(0);
    std::vector<std::byte> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), (std::streamsize) size);
    if (!in) {
        return std::unexpected(std::format("cannot read {}", path));
    }
    return bytes;
}

auto writePackerFile(const std::string& path, std::span<const std::byte> bytes) -> std::expected<void, std::string> {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize) bytes.size());
    if (!out) {
        return std::unexpected(std::format("cannot write {}", path));
    }
    return {};
}

auto secondaryOutputFile(const std::string& out, const std::string& fragment) -> std::string {
    std::string name = fragment;
    std::ranges::replace(name, '/', '.');
    return out + "#" + name;
}

auto secondaryOutputsList(const std::string& out) -> std::string {
    return out + ".outputs";
}

auto clearSecondaryOutputs(const std::string& out) -> void {
    std::error_code ec;
    for (const auto& [id, file] : readSecondaryOutputs(out)) {
        std::filesystem::remove(file, ec);
    }
    std::filesystem::remove(secondaryOutputsList(out), ec);
}

auto writeSecondaryOutputs(const std::string& out, const std::vector<std::pair<std::string, std::string>>& outputs) -> std::expected<void, std::string> {
    auto path = secondaryOutputsList(out);
    std::ofstream list(path, std::ios::trunc);
    for (const auto& [id, file] : outputs) {
        list << id << '\t' << file << '\n';
    }
    if (!list) {
        return std::unexpected(std::format("cannot write {}", path));
    }
    return {};
}

auto readSecondaryOutputs(const std::string& out) -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> outputs;
    std::ifstream list(secondaryOutputsList(out));
    std::string line;
    while (std::getline(list, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        outputs.emplace_back(line.substr(0, tab), line.substr(tab + 1));
    }
    return outputs;
}
