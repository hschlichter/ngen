// ngen-build-run CLI entry point.
//
// Thin wrapper: parses argv, loads an IR from `--ir <path>` via `build::ir::read`, fills in defaults
// (`-j` falls back to `std::thread::hardware_concurrency()` when zero or absent), and calls
// `ngen::run::execute()`. All real work lives in `execute.hpp`.
//
// Exit codes: 0 on success; 1 on argument error, missing IR, version mismatch, or any failed edge; 130 on
// SIGINT (interrupted build). The `bootstrap.cpp` orchestrator forwards these so the user-facing `ngen-build`
// echoes the runner's status.
//
// This binary is reachable from `bootstrap.cpp` for the *project* build (one subprocess invocation per
// `./_out/ngen-build` run). The *build-system self-build* path does not go through this binary — it calls
// `ngen::run::execute()` directly from within `ngen-build` itself, in-process.

#include "../ir/reader.hpp"
#include "execute.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

struct Args {
    std::string ir_path;
    ngen::run::RunOptions options;
};

auto parse(int argc, char** argv) -> std::expected<Args, build::Error> {
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        auto next = [&](std::string_view name) -> std::expected<std::string, build::Error> {
            if (i + 1 >= argc) {
                return std::unexpected(build::Error{std::string("missing value for ") + std::string(name)});
            }
            return std::string(argv[++i]);
        };
        if (arg == "--ir") {
            auto v = next("--ir");
            if (!v) {
                return std::unexpected(v.error());
            }
            args.ir_path = *v;
        } else if (arg == "-j") {
            auto v = next("-j");
            if (!v) {
                return std::unexpected(v.error());
            }
            try {
                args.options.jobs = std::stoi(*v);
            } catch (...) {
                return std::unexpected(build::Error{"-j expects an integer"});
            }
        } else if (arg == "-k") {
            auto v = next("-k");
            if (!v) {
                return std::unexpected(v.error());
            }
            try {
                args.options.keep_going = std::stoi(*v);
            } catch (...) {
                return std::unexpected(build::Error{"-k expects an integer"});
            }
        } else if (arg == "-v" || arg == "--verbose") {
            args.options.verbose = true;
        } else if (arg == "-vv") {
            args.options.very_verbose = true;
        } else {
            args.options.targets.emplace_back(arg);
        }
    }
    if (args.ir_path.empty()) {
        return std::unexpected(build::Error{"--ir <path> is required"});
    }
    return args;
}

} // namespace

auto main(int argc, char** argv) -> int {
    auto args = parse(argc, argv);
    if (!args) {
        std::cerr << args.error().message << "\n";
        return 1;
    }

    auto ir = build::ir::read(args->ir_path);
    if (!ir) {
        std::cerr << ir.error().message << "\n";
        return 1;
    }

    args->options.ir_path = args->ir_path;
    if (args->options.jobs <= 0) {
        auto hc = std::thread::hardware_concurrency();
        args->options.jobs = static_cast<int>(hc == 0 ? 1u : hc);
    }
    auto result = ngen::run::execute(*ir, args->options);
    if (!result) {
        std::cerr << result.error().message << "\n";
        return 1;
    }
    if (result->interrupted) {
        std::cerr << "interrupted\n";
        return 130;
    }
    if (result->failures > 0) {
        std::cerr << result->failures << " edge(s) failed (of " << result->total_edges << ")\n";
        return 1;
    }
    return 0;
}
