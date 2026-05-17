#include <algorithm>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Error {
    std::string message;
};

auto write_if_changed(const std::filesystem::path& path, const std::string& text) -> std::expected<void, Error> {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return std::unexpected(Error{"failed to create directory " + path.parent_path().string() + ": " + ec.message()});
    }
    std::ifstream in(path);
    std::ostringstream current;
    current << in.rdbuf();
    if (in && current.str() == text) {
        return {};
    }
    std::ofstream out(path);
    if (!out) {
        return std::unexpected(Error{"failed to open " + path.string() + " for writing"});
    }
    out << text;
    if (!out) {
        return std::unexpected(Error{"failed to write " + path.string()});
    }
    return {};
}

auto shell_quote(const std::string& value) -> std::string {
    if (value.empty()) {
        return "''";
    }
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

struct Args {
    std::string target = "ngen-view";
    std::string platform;
    std::string config;
    int verbosity = 0;
    bool list = false;
    bool dump_graph = false;
};

auto parse(int argc, char** argv) -> std::expected<Args, Error> {
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&]() -> std::expected<std::string, Error> {
            if (i + 1 >= argc) {
                return std::unexpected(Error{"missing value for " + arg});
            }
            return std::string(argv[++i]);
        };
        if (arg == "--platform") {
            auto parsed = value();
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            args.platform = *parsed;
        } else if (arg == "--config" || arg == "-c") {
            auto parsed = value();
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            args.config = *parsed;
        } else if (arg == "-v" || arg == "--verbose") {
            args.verbosity = std::max(args.verbosity, 1);
        } else if (arg == "-vv") {
            args.verbosity = std::max(args.verbosity, 2);
        } else if (arg == "--list") {
            args.list = true;
        } else if (arg == "--dump-graph") {
            args.dump_graph = true;
        } else {
            args.target = arg;
        }
    }
    return args;
}

auto graph_forward_args(int argc, char** argv) -> std::string {
    std::string out;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose" || arg == "-vv") {
            continue;
        }
        out += " ";
        out += shell_quote(argv[i]);
    }
    return out;
}

auto chosen_variant(const Args& args) -> std::pair<std::string, std::string> {
    auto platform = args.platform.empty() ? std::string("linux-vulkan") : args.platform;
    auto config = args.config.empty() ? std::string("debug") : args.config;
    return {platform, config};
}

} // namespace

auto main(int argc, char** argv) -> int {
    auto args = parse(argc, argv);
    if (!args) {
        std::cerr << args.error().message << "\n";
        return 1;
    }

    auto written = write_if_changed("_out/ngen-build-pre.ninja", R"(cxx = clang++
cxxflags = -std=c++23 -O0 -g -Wall -Wextra
builddir = _out/.ninja

rule cxx
  command = mkdir -p _out && $cxx $cxxflags -o $out $in
  description = PREBUILD $out

build _out/ngen-build-pre: cxx build/prebuild.cpp

default _out/ngen-build-pre
)");
    if (!written) {
        std::cerr << written.error().message << "\n";
        return 1;
    }

    auto prebuild_cmd = std::string(args->verbosity == 1 ? "TERM=dumb ninja -f _out/ngen-build-pre.ninja" : "ninja -f _out/ngen-build-pre.ninja");
    if (args->verbosity >= 2) {
        prebuild_cmd += " -v";
    }
    // Orchestrator stages must shell out to ninja; std::system is intentional here.
    if (std::system(prebuild_cmd.c_str()) != 0) { // NOLINT(bugprone-command-processor)
        return 1;
    }
    if (std::system("./_out/ngen-build-pre") != 0) { // NOLINT(bugprone-command-processor)
        return 1;
    }

    auto graph_cmd = "./_out/ngen-build-graph" + graph_forward_args(argc, argv);
    if (std::system(graph_cmd.c_str()) != 0) { // NOLINT(bugprone-command-processor)
        return 1;
    }

    if (args->list || args->dump_graph) {
        return 0;
    }

    auto [platform, config] = chosen_variant(*args);
    auto ir_path = "_out/" + platform + "/" + config + "/build.ngenir";
    std::string cmd = "./_out/ngen-build-run --ir " + shell_quote(ir_path);
    if (args->verbosity == 1) {
        cmd = "TERM=dumb " + cmd + " -v";
    }
    if (args->verbosity >= 2) {
        cmd += " -vv";
    }
    cmd += " " + shell_quote(args->target);
    return std::system(cmd.c_str()) == 0 ? 0 : 1; // NOLINT(bugprone-command-processor)
}
