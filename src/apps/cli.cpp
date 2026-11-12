// ngen-cli: one front door to the engine's tools.
//
//   ngen-cli set <platform> <config>   make a variant the set one (unique prefixes accepted: `set linux release`)
//   ngen-cli set                       print the set variant
//   ngen-cli build [ngen-build args]   ngen-build, with -p/-c filled in from the set variant where not given
//   ngen-cli view [ngen-view args]     the set variant's ngen-view
//   ngen-cli introspect [args]         the set variant's ngen-introspect (window; list, get, describe, call)
//   ngen-cli asset-server              the set variant's ngen-asset-server, in the foreground; ngen-view needs it running
//   ngen-cli pack <scene>...           the set variant's ngen-pack: the asset server packs everything the scenes can reach
//
// The set variant lives in `_out/set` as one line, `<platform>/<config>`: the same two components as the variant's output directory, so a
// tool of the set variant is `_out/<that line>/<tool>`. `set` also points the repository-root `ngen-cli` at the set variant's own cli,
// building it first so it never points at a missing binary. On Linux that is a symlink; on Windows, where a symlink needs administrator
// rights or Developer Mode, it is a copy, `ngen-cli.exe`.
//
// ngen-cli is an ordinary build target, built per variant. First use on a fresh clone, after bootstrapping ngen-build:
//
//     ./_out/ngen-build -p linux-vulkan -c debug ngen-cli && ./_out/linux-vulkan/debug/ngen-cli set linux-vulkan debug
//
// Forwarded tools run in the caller's working directory and environment, and their exit code is ours. On Linux they replace this process
// (execv); Windows has no exec, so there the cli starts them with its own console and waits.

#include "os.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

struct Variant {
    std::string platform;
    std::string config;
};

struct VariantNames {
    std::vector<std::string> platforms;
    std::vector<std::string> configs;
};

// Commands that run a tool of the set variant. Adding a tool is a row here.
struct ToolCommand {
    std::string_view command;
    std::string_view binary;
    std::string_view summary;
};

constexpr std::array<ToolCommand, 4> toolCommands = {{
    {.command = "view", .binary = "ngen-view", .summary = "run the viewer"},
    {.command = "introspect", .binary = "ngen-introspect", .summary = "see and call running tools: records, methods (no arguments: the window)"},
    {.command = "asset-server", .binary = "ngen-asset-server", .summary = "serve assets to the viewer (run it before `view`)"},
    {.command = "pack", .binary = "ngen-pack", .summary = "pack everything a scene can reach, so the viewer opens it from the cache"},
}};

// This binary is <root>/_out/<platform>/<config>/ngen-cli; the root link resolves to it too. The root copy on Windows sits in the root
// itself, beside _out/.
auto repoRoot() -> fs::path {
    auto directory = os::executablePath().value_or(fs::path()).parent_path();
    std::error_code ec;
    if (fs::is_directory(directory / "_out", ec)) {
        return directory;
    }
    return directory.parent_path().parent_path().parent_path();
}

auto programName(std::string_view stem) -> std::string {
    return std::string(stem) + std::string(os::executableSuffix);
}

auto buildTool(const fs::path& root) -> fs::path {
    return root / "_out" / programName("ngen-build");
}

auto variantPath(const Variant& v) -> std::string {
    return v.platform + "/" + v.config;
}

auto readSet(const fs::path& root) -> std::optional<Variant> {
    std::ifstream file(root / "_out" / "set");
    std::string line;
    if (!file || !std::getline(file, line)) {
        return std::nullopt;
    }
    auto slash = line.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= line.size()) {
        return std::nullopt;
    }
    return Variant{.platform = line.substr(0, slash), .config = line.substr(slash + 1)};
}

auto writeSet(const fs::path& root, const Variant& v) -> bool {
    std::ofstream file(root / "_out" / "set", std::ios::trunc);
    file << variantPath(v) << '\n';
    return (bool) file;
}

// The platform and configuration sections of `ngen-build -l`: a header line, then one indented name per line.
auto listVariants(const fs::path& root) -> std::expected<VariantNames, std::string> {
    auto listed = os::runProcess({buildTool(root).string(), "-l"});
    if (listed.exitCode != 0) {
        return std::unexpected("cannot run " + buildTool(root).string() + " -l");
    }
    VariantNames names;
    std::vector<std::string>* section = nullptr;
    std::string_view rest = listed.output;
    while (!rest.empty()) {
        auto newline = rest.find('\n');
        std::string_view line = rest.substr(0, newline);
        rest = newline == std::string_view::npos ? std::string_view() : rest.substr(newline + 1);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.remove_suffix(1);
        }
        if (line == "Platforms:") {
            section = &names.platforms;
        } else if (line == "Configurations:") {
            section = &names.configs;
        } else if (line.empty() || line.front() != ' ') {
            section = nullptr;
        } else if (section != nullptr) {
            auto start = line.find_first_not_of(' ');
            auto end = line.find(' ', start);
            section->emplace_back(line.substr(start, end - start));
        }
    }
    if (names.platforms.empty() || names.configs.empty()) {
        return std::unexpected(buildTool(root).string() + " -l did not list platforms and configurations");
    }
    return names;
}

auto joined(std::span<const std::string> names) -> std::string {
    std::string text;
    for (const auto& name : names) {
        if (!text.empty()) {
            text += ", ";
        }
        text += name;
    }
    return text;
}

// Exact name first, then a unique prefix.
auto resolveName(std::string_view query, std::span<const std::string> names, std::string_view what) -> std::expected<std::string, std::string> {
    for (const auto& name : names) {
        if (name == query) {
            return name;
        }
    }
    std::vector<std::string> matches;
    for (const auto& name : names) {
        if (name.starts_with(query)) {
            matches.push_back(name);
        }
    }
    if (matches.size() == 1) {
        return matches.front();
    }
    if (matches.empty()) {
        return std::unexpected(std::format("unknown {} '{}'; valid: {}", what, query, joined(names)));
    }
    return std::unexpected(std::format("ambiguous {} '{}'; matches: {}", what, query, joined(matches)));
}

// Runs a tool as a child and returns its exit code.
auto runTool(const fs::path& program, std::span<const std::string> args) -> int {
    auto code = os::runProcessInteractive(program, args);
    if (code == 127 && !fs::exists(program)) {
        std::println(stderr, "ngen-cli: cannot run {}", program.string());
    }
    return code;
}

#if defined(_WIN32)
// No exec on Windows: the tool runs as a child on this console, and its exit code is returned.
auto execTool(const fs::path& program, std::span<const std::string> args) -> int {
    return runTool(program, args);
}

// A running program's file cannot be overwritten on Windows, so a build that relinks this cli would fail. It can be renamed: while a build
// runs, this file is moved aside, and put back if the build did not write a new one. The moved-aside copy is deleted once it is not running.
class SelfAside {
public:
    SelfAside() {
        auto exe = os::executablePath();
        if (!exe) {
            return;
        }
        self = *exe;
        aside = self;
        aside += ".old";
        std::error_code ec;
        fs::remove(aside, ec);
        fs::rename(self, aside, ec);
        if (ec) {
            self.clear();
        }
    }
    SelfAside(const SelfAside&) = delete;
    auto operator=(const SelfAside&) -> SelfAside& = delete;
    ~SelfAside() {
        if (self.empty()) {
            return;
        }
        std::error_code ec;
        if (!fs::exists(self, ec)) {
            fs::rename(aside, self, ec);
        }
    }

private:
    fs::path self;
    fs::path aside;
};
#else
// Linux replaces a running program's file without trouble.
class SelfAside {
public:
    SelfAside() {}
};

// Replaces this process. Returns only on failure.
auto execTool(const fs::path& program, std::span<const std::string> args) -> int {
    std::vector<char*> argv;
    // execv takes char* const[]; it does not write through them.
    argv.push_back(const_cast<char*>(program.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }
    argv.push_back(nullptr);
    execv(program.c_str(), argv.data());
    std::println(stderr, "ngen-cli: cannot run {}: {}", program.string(), std::strerror(errno));
    return 127;
}
#endif

auto requireBuildTool(const fs::path& root) -> bool {
    if (fs::exists(buildTool(root))) {
        return true;
    }
    std::println(stderr, "ngen-cli: {} is missing; bootstrap it first:", buildTool(root).string());
#if defined(_WIN32)
    std::println(stderr, "  mkdir -p _out && clang++ -std=c++23 -O0 -g -D_CRT_SECURE_NO_WARNINGS -o _out/ngen-build.exe src/build/bootstrap.cpp");
#else
    std::println(stderr, "  mkdir -p _out && c++ -std=c++23 -O0 -g -pthread -o _out/ngen-build src/build/bootstrap.cpp");
#endif
    return false;
}

#if defined(_WIN32)
// Copies the variant's cli to <root>/ngen-cli.exe. A running program's file cannot be overwritten or deleted on Windows, but it can be
// renamed: the old copy (perhaps this very process) moves aside first and is deleted the next time it is not running.
auto linkRoot(const fs::path& root, const Variant& v) -> bool {
    auto link = root / programName("ngen-cli");
    auto aside = root / ".ngen-cli.old.exe";
    std::error_code ec;
    fs::remove(aside, ec);
    if (fs::exists(link, ec)) {
        fs::rename(link, aside, ec);
        if (ec) {
            std::println(stderr, "ngen-cli: cannot move {} aside: {}", link.string(), ec.message());
            return false;
        }
    }
    fs::copy_file(root / "_out" / v.platform / v.config / programName("ngen-cli"), link, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::println(stderr, "ngen-cli: cannot copy the cli to {}: {}", link.string(), ec.message());
        return false;
    }
    fs::remove(aside, ec);
    return true;
}
#else
// Points <root>/ngen-cli at the variant's cli, replacing the old link in one rename.
auto linkRoot(const fs::path& root, const Variant& v) -> bool {
    auto link = root / "ngen-cli";
    std::error_code ec;
    if (fs::exists(fs::symlink_status(link)) && !fs::is_symlink(fs::symlink_status(link))) {
        std::println(stderr, "ngen-cli: {} exists and is not a symlink; not replacing it", link.string());
        return false;
    }
    auto temporary = root / ".ngen-cli.link";
    fs::remove(temporary, ec);
    fs::create_symlink(fs::path("_out") / v.platform / v.config / "ngen-cli", temporary, ec);
    if (ec) {
        std::println(stderr, "ngen-cli: cannot create {}: {}", temporary.string(), ec.message());
        return false;
    }
    fs::rename(temporary, link, ec);
    if (ec) {
        std::println(stderr, "ngen-cli: cannot replace {}: {}", link.string(), ec.message());
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}
#endif

auto commandSet(const fs::path& root, std::span<const std::string> args) -> int {
    if (args.empty()) {
        auto set = readSet(root);
        if (!set.has_value()) {
            std::println(stderr, "no variant set; run: ngen-cli set <platform> <config>");
            return 1;
        }
        std::println("{}", variantPath(*set));
        return 0;
    }
    if (args.size() != 2) {
        std::println(stderr, "usage: ngen-cli set <platform> <config>");
        return 1;
    }
    if (!requireBuildTool(root)) {
        return 1;
    }
    auto names = listVariants(root);
    if (!names.has_value()) {
        std::println(stderr, "ngen-cli: {}", names.error());
        return 1;
    }
    auto platform = resolveName(args[0], names->platforms, "platform");
    auto config = resolveName(args[1], names->configs, "config");
    if (!platform.has_value() || !config.has_value()) {
        if (!platform.has_value()) {
            std::println(stderr, "ngen-cli: {}", platform.error());
        }
        if (!config.has_value()) {
            std::println(stderr, "ngen-cli: {}", config.error());
        }
        return 1;
    }
    Variant variant = {.platform = *platform, .config = *config};
    // The variant's own cli first, so the root link never points at a missing binary.
    std::vector<std::string> buildArgs = {"-p", variant.platform, "-c", variant.config, "ngen-cli"};
    int code = 0;
    {
        SelfAside aside;
        code = runTool(buildTool(root), buildArgs);
    }
    if (code != 0) {
        std::println(stderr, "ngen-cli: building ngen-cli for {} failed; set variant unchanged", variantPath(variant));
        return code;
    }
    if (!writeSet(root, variant)) {
        std::println(stderr, "ngen-cli: cannot write {}", (root / "_out" / "set").string());
        return 1;
    }
    if (!linkRoot(root, variant)) {
        return 1;
    }
    std::println("set {}", variantPath(variant));
    return 0;
}

auto hasFlag(std::span<const std::string> args, std::string_view shortFlag, std::string_view longFlag) -> bool {
    auto longWithValue = std::string(longFlag) + "=";
    return std::ranges::any_of(args, [&](const std::string& arg) {
        return arg == shortFlag || arg == longFlag || arg.starts_with(longWithValue);
    });
}

auto commandBuild(const fs::path& root, std::span<const std::string> args) -> int {
    if (!requireBuildTool(root)) {
        return 1;
    }
    bool hasPlatform = hasFlag(args, "-p", "--platform");
    bool hasConfig = hasFlag(args, "-c", "--config");
    std::vector<std::string> forwarded;
    if (!hasPlatform || !hasConfig) {
        auto set = readSet(root);
        if (!set.has_value()) {
            std::println(stderr, "ngen-cli: no variant set; run `ngen-cli set <platform> <config>` or pass -p/-c");
            return 1;
        }
        if (!hasPlatform) {
            forwarded.emplace_back("-p");
            forwarded.push_back(set->platform);
        }
        if (!hasConfig) {
            forwarded.emplace_back("-c");
            forwarded.push_back(set->config);
        }
    }
    forwarded.insert(forwarded.end(), args.begin(), args.end());
    SelfAside aside;
    return execTool(buildTool(root), forwarded);
}

auto commandTool(const fs::path& root, const ToolCommand& tool, std::span<const std::string> args) -> int {
    auto set = readSet(root);
    if (!set.has_value()) {
        std::println(stderr, "ngen-cli: no variant set; run: ngen-cli set <platform> <config>");
        return 1;
    }
    auto program = root / "_out" / set->platform / set->config / programName(tool.binary);
    if (!fs::exists(program)) {
        std::println(stderr, "ngen-cli: {} is not built for {}; run: ngen-cli build {}", tool.binary, variantPath(*set), tool.binary);
        return 1;
    }
    return execTool(program, args);
}

auto printHelp(const fs::path& root) -> void {
    std::println("usage: ngen-cli <command> [args]");
    std::println("");
    std::println("  set <platform> <config>   make a variant the set one (prefixes accepted)");
    std::println("  set                       print the set variant");
    std::println("  build [args]              ngen-build; -p/-c default to the set variant");
    for (const auto& tool : toolCommands) {
        std::println("  {:<25} {} ({})", std::string(tool.command) + " [args]", tool.summary, tool.binary);
    }
    std::println("  help                      this message");
    std::println("");
    auto set = readSet(root);
    if (!set.has_value()) {
        std::println("no variant set");
        return;
    }
    std::println("set variant: {}", variantPath(*set));
    for (const auto& tool : toolCommands) {
        auto built = fs::exists(root / "_out" / set->platform / set->config / programName(tool.binary));
        std::println("  {}: {}", tool.binary, built ? "built" : "not built");
    }
}

} // namespace

auto main(int argc, char** argv) -> int {
    auto root = repoRoot();
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "help" || args[0] == "-h" || args[0] == "--help") {
        printHelp(root);
        return 0;
    }
    auto command = args[0];
    auto rest = std::span<const std::string>(args).subspan(1);
    if (command == "set") {
        return commandSet(root, rest);
    }
    if (command == "build") {
        return commandBuild(root, rest);
    }
    for (const auto& tool : toolCommands) {
        if (command == tool.command) {
            return commandTool(root, tool, rest);
        }
    }
    std::println(stderr, "ngen-cli: unknown command '{}'; see ngen-cli help", command);
    return 1;
}
