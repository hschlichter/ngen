// ngen-build — the user-facing orchestrator binary.
//
// `bootstrap.cpp` is the only `.cpp` file a fresh-clone contributor compiles by hand:
//
//     mkdir -p _out && c++ -std=c++23 -O0 -g -pthread -o _out/ngen-build build/bootstrap.cpp
//
// (Documented in `CLAUDE.md` and `build/build_system.md`.) The resulting binary then drives every subsequent build.
// Three things happen in order on each invocation:
//
//   1. **Self-build.** `self_build_ir()` constructs an in-memory `build::ir::IR` with two edges — one to
//      compile `build.cpp` into `_out/ngen-build-graph`, one to compile `build/run/main.cpp` into
//      `_out/ngen-build-run`. `ngen::run::execute()` runs the IR in-process against the build log at
//      `_out/.system/.ngen-buildlog`, so the two binaries are recompiled only when their sources or
//      depfile-tracked headers actually change. This is the seam where the build system is "self-hosted": the
//      runner that builds the project also builds the binaries the build system itself uses.
//
//   2. **Graph stage.** `./_out/ngen-build-graph` is invoked as a subprocess. It walks the `Project` defined
//      in `build.cpp`, runs `ir::Emitter`, and writes one `_out/<platform>/<config>/build.ngenir` per
//      variant plus the per-variant and merged `compile_commands.json`. Short-circuits with `--list` /
//      `--dump-graph` if the user asked for those.
//
//   3. **Project run.** Positional target arguments are first run through `ir::resolve_target` — exact names
//      pass through, otherwise the resolver fuzzy-matches against ObjectFile source stems and then non-OF edge
//      names, expanding one query into the full set of matching edges. The resolved list is then handed to
//      `./_out/ngen-build-run --ir _out/<platform>/<config>/build.ngenir <target>...` as a subprocess. The
//      runner loads the IR, computes the dirty set against `_out/<plat>/<cfg>/.ngen-buildlog`, and runs the
//      dirty edges in parallel.
//
// Platform / config selection: `--platform <name>` / `--config <name>`, both required. There are no defaults
// here — the build-system code under `build/` carries zero project knowledge, so it cannot pick a sensible
// platform or config on the user's behalf. When either flag is missing, the orchestrator invokes the graph
// stage's `--list` (which prints registered platforms, configs, and top-level targets via
// `build::print_summary`) and then reports the missing flag. Targets are positional and may be repeated;
// empty means "use the IR's default_targets". Verbosity (`-v`, `-vv`) is forwarded to the runner; `--list`
// and `--dump-graph` are forwarded to the graph stage.
//
// `std::system` is used to spawn the two subprocesses (graph and runner). It's marked with
// `// NOLINT(bugprone-command-processor)` since this is the deliberate orchestration boundary; the in-process
// self-build path uses the runner library directly without `std::system`.

#include "ir/reader.hpp"
#include "ir/resolve.hpp"
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
    std::vector<std::string> targets; // empty = let the runner use the IR's default_targets
    std::string platform;
    std::string config;
    int verbosity = 0;
    bool list = false;
    bool dump_graph = false;
    bool help = false;
    bool clean = false;
    bool rebuild = false;
    bool compile_commands = false;
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
        if (arg == "--platform" || arg == "-p") {
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
        } else if (arg == "--list" || arg == "-l") {
            args.list = true;
        } else if (arg == "--dump-graph") {
            args.dump_graph = true;
        } else if (arg == "--help" || arg == "-h") {
            args.help = true;
        } else if (arg == "--clean") {
            args.clean = true;
        } else if (arg == "--rebuild") {
            args.rebuild = true;
        } else if (arg == "--compile-commands") {
            args.compile_commands = true;
        } else {
            args.targets.emplace_back(arg);
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

// The flag list ngen-build itself accepts; the project section (platforms / configs / targets) below comes
// from the graph stage via `--list`. Always goes to stdout — the missing-args fallback in `main()` follows
// this with its own stderr error line.
auto print_help() -> void {
    std::cout << "Usage: ngen-build [options] [target]\n"
              << "\n"
              << "Options:\n"
              << "  -p, --platform <name>   Build for this platform.    (required for builds)\n"
              << "  -c, --config <name>     Build with this config.     (required for builds)\n"
              << "      --clean             Remove the variant's build outputs (requires -p / -c); exit.\n"
              << "      --rebuild           --clean, then build from scratch (requires -p / -c).\n"
              << "      --compile-commands  Write compile_commands.json for the variant + merged top-level union\n"
              << "                          (requires -p / -c).\n"
              << "  -v, --verbose           One line per edge; same effect as TERM=dumb.\n"
              << "  -vv                     Echo each shell command before running.\n"
              << "  -l, --list              Show available platforms, configs, and targets; exit.\n"
              << "      --dump-graph        Print the project IR as JSON to stdout; exit.\n"
              << "  -h, --help              Show this message.\n"
              << "\n"
              << "Targets are positional and may repeat; empty means \"use the project's default_target\".\n"
              << "A target is matched first by exact edge name, then by case-insensitive substring against\n"
              << "ObjectFile source stems (every hit is built), then by substring against library / program /\n"
              << "tool / alias names. Unmatched queries are passed through and produce an unknown-target error.\n"
              << "\n"
              << std::flush;
    // Shell out to the graph stage for the project-specific listing. `std::system` writes directly to fd 1
    // and does not see the iostream buffer, so flushing before the call is required to preserve order.
    std::system("./_out/ngen-build-graph --list"); // NOLINT(bugprone-command-processor)
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
        edge.command = "c++ " + cxxflags + " -MMD -MF _out/ngen-build-graph.d -o _out/ngen-build-graph build.cpp";
        edge.inputs = {"build.cpp"};
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

    if (args->help) {
        print_help();
        return 0;
    }

    // --clean / --rebuild both wipe the variant directory and require explicit -p / -c. Handle them before the
    // graph stage runs: --clean exits, --rebuild falls through to the regular pipeline which will re-emit the
    // IR and rebuild everything from scratch.
    if (args->clean || args->rebuild) {
        if (args->platform.empty() || args->config.empty()) {
            print_help();
            std::cerr << "\n"
                      << "Error: " << (args->clean ? "--clean" : "--rebuild")
                      << " requires --platform and --config.\n";
            return 1;
        }
        auto safe = [](const std::string& s) {
            return !s.empty() && s.find('/') == std::string::npos && s.find('\\') == std::string::npos && s.find("..") == std::string::npos;
        };
        if (!safe(args->platform) || !safe(args->config)) {
            std::cerr << "Error: invalid characters in --platform / --config (no `/`, `\\`, or `..`).\n";
            return 1;
        }
        auto variant_dir = std::filesystem::path("_out") / args->platform / args->config;
        std::error_code ec;
        std::filesystem::remove_all(variant_dir, ec);
        if (ec) {
            std::cerr << "Error: failed to remove " << variant_dir.string() << ": " << ec.message() << "\n";
            return 1;
        }
        if (args->clean) {
            return 0;
        }
    }

    auto graph_cmd = "./_out/ngen-build-graph" + graph_forward_args(argc, argv);
    if (std::system(graph_cmd.c_str()) != 0) { // NOLINT(bugprone-command-processor)
        return 1;
    }

    if (args->list || args->dump_graph || args->compile_commands) {
        return 0;
    }

    if (args->platform.empty() || args->config.empty()) {
        // Friendly fallback: the build system has no project knowledge to fall back on, so show the same panel
        // `--help` would print and then report what was missing on stderr.
        print_help();
        std::cerr << "\n"
                  << "Error: --platform and --config are required.\n";
        return 1;
    }

    auto ir_path = "_out/" + args->platform + "/" + args->config + "/build.ngenir";

    // Resolve each positional target against the chosen variant's IR. Exact names pass through unchanged;
    // unmatched strings also pass through (runner will produce its own error). Fuzzy matches fan out into
    // one or more concrete edge names — `ir::resolve_target` is the single source of truth for the rule.
    std::vector<std::string> resolved_targets;
    if (!args->targets.empty()) {
        auto ir = build::ir::read(build::Path(ir_path));
        if (!ir) {
            std::cerr << ir.error().message << "\n";
            return 1;
        }
        for (const auto& query : args->targets) {
            auto matches = build::ir::resolve_target(*ir, query);
            if (matches.empty()) {
                resolved_targets.push_back(query);
                continue;
            }
            resolved_targets.insert(resolved_targets.end(), matches.begin(), matches.end());
        }
    }

    std::string cmd = "./_out/ngen-build-run --ir " + shell_quote(ir_path);
    if (args->verbosity == 1) {
        cmd = "TERM=dumb " + cmd + " -v";
    }
    if (args->verbosity >= 2) {
        cmd += " -vv";
    }
    for (const auto& t : resolved_targets) {
        cmd += " " + shell_quote(t);
    }
    return std::system(cmd.c_str()) == 0 ? 0 : 1; // NOLINT(bugprone-command-processor)
}
