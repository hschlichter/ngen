#include "ir/schema.hpp"
#include "run/execute.hpp"

#include <algorithm>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {

struct Args {
    std::string target = "ngen-view";
    std::string platform;
    std::string config;
    int verbosity = 0;
    bool list = false;
    bool dump_graph = false;
};

auto parse(int argc, char** argv) -> std::expected<Args, build::Error> {
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&]() -> std::expected<std::string, build::Error> {
            if (i + 1 >= argc) {
                return std::unexpected(build::Error{"missing value for " + arg});
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

// In-memory IR describing how to compile the two binaries that drive the rest
// of the build (`ngen-build-graph` and `ngen-build-run`). The runner library
// turns this into either a no-op (binaries fresh) or a parallel recompile.
auto self_build_ir() -> build::ir::IR {
    build::ir::IR ir;
    ir.variant = "system";
    ir.project_root = std::filesystem::current_path().string();
    ir.pools = build::ir::make_default_pools();

    auto cxxflags = std::string("-std=c++23 -O0 -g -Wall -Wextra -pthread");

    {
        build::ir::Edge edge;
        edge.name = "ngen-build-graph";
        edge.command = "c++ " + cxxflags + " -MMD -MF _out/ngen-build-graph.d -o _out/ngen-build-graph build/build.cpp";
        edge.inputs = {"build/build.cpp"};
        edge.outputs = {"_out/ngen-build-graph"};
        edge.depfile = "_out/ngen-build-graph.d";
        edge.description = "GRAPH _out/ngen-build-graph";
        edge.pool = build::ir::kPoolDefault;
        ir.edges.push_back(std::move(edge));
    }
    {
        build::ir::Edge edge;
        edge.name = "ngen-build-run";
        edge.command = "c++ " + cxxflags + " -MMD -MF _out/ngen-build-run.d -o _out/ngen-build-run build/run/main.cpp";
        edge.inputs = {"build/run/main.cpp"};
        edge.outputs = {"_out/ngen-build-run"};
        edge.depfile = "_out/ngen-build-run.d";
        edge.description = "RUNNER _out/ngen-build-run";
        edge.pool = build::ir::kPoolDefault;
        ir.edges.push_back(std::move(edge));
    }
    ir.default_targets = {0, 1};
    return ir;
}

auto run_self_build(const Args& args) -> bool {
    auto ir = self_build_ir();
    ngen::run::RunOptions opts;
    // Synthetic IR path: execute() derives the build log location from it
    // (_out/.system/.ngen-buildlog) but never reads or writes the IR file itself.
    opts.ir_path = "_out/.system/build.ngenir";
    auto hc = std::thread::hardware_concurrency();
    opts.jobs = static_cast<int>(hc == 0 ? 2u : hc);
    if (args.verbosity >= 2) {
        opts.very_verbose = true;
    } else if (args.verbosity >= 1) {
        opts.verbose = true;
    }
    auto result = ngen::run::execute(ir, opts);
    if (!result) {
        std::cerr << "self-build failed: " << result.error().message << "\n";
        return false;
    }
    if (result->failures > 0) {
        std::cerr << result->failures << " self-build edge(s) failed (of " << result->total_edges << ")\n";
        return false;
    }
    return true;
}

} // namespace

auto main(int argc, char** argv) -> int {
    auto args = parse(argc, argv);
    if (!args) {
        std::cerr << args.error().message << "\n";
        return 1;
    }

    if (!run_self_build(*args)) {
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
