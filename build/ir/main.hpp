// build::ir::main — the graph-stage `main()` body, parameterised over the project.
//
// `build.cpp` constructs the user's `Project` and ends with `return ir::main(argc, argv, p);`. Everything in
// this file is project-agnostic CLI plumbing: parse argv, dispatch to `print_summary` for `--list`, to
// `Emitter::dump` for `--dump-graph`, to `Emitter::emit` for a normal run, and (when `--compile-commands` is
// passed) to `compile_command_entries` plus a small JSON-array wrap for the on-demand IDE-integration file.
//
// Lives in `ir/` because it's the entry point for the IR-emitter binary (`ngen-build-graph`). Sits at the same
// layer as the `ngen::run::execute()` entry point for the runner.

#pragma once

#include "../framework/glob.hpp"
#include "../framework/inspect.hpp"
#include "../framework/project.hpp"
#include "../framework/path.hpp"
#include "compile_commands.hpp"
#include "emit.hpp"
#include "reader.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace build::ir {

namespace detail {

inline auto wrap_compile_commands_array(const std::vector<std::string>& entries) -> std::string {
    std::string out = "[\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        out += "  ";
        out += entries[i];
        if (i + 1 < entries.size()) {
            out += ",";
        }
        out += "\n";
    }
    out += "]\n";
    return out;
}

inline auto write_compile_commands(const Project& project, const Path& root_out_dir, std::string_view chosen_platform,
                                   std::string_view chosen_config) -> std::expected<void, Error> {
    if (chosen_platform.empty() || chosen_config.empty()) {
        return std::unexpected(Error{"--compile-commands requires --platform / -p and --config / -c"});
    }

    std::vector<std::string> merged;
    bool wrote_chosen = false;

    for (const auto* p : project.platforms()) {
        for (const auto* c : project.configs()) {
            auto ir_path = root_out_dir / p->name() / c->name() / build::Path("build.ngenir");
            std::error_code ec;
            if (!std::filesystem::exists(ir_path.value, ec)) {
                continue;
            }
            auto ir = read(ir_path);
            if (!ir) {
                continue; // best-effort: skip unreadable variants but keep going for the rest
            }
            auto entries = compile_command_entries(*ir);

            if (p->name() == chosen_platform && c->name() == chosen_config) {
                auto written = write_if_changed(ir_path.parent_path() / build::Path("compile_commands.json"), wrap_compile_commands_array(entries));
                if (!written) {
                    return std::unexpected(written.error());
                }
                wrote_chosen = true;
            }
            merged.insert(merged.end(), entries.begin(), entries.end());
        }
    }

    if (!wrote_chosen) {
        return std::unexpected(Error{"--compile-commands: chosen variant " + std::string(chosen_platform) + "/" + std::string(chosen_config) +
                                     " has no IR on disk (was it cleaned?)"});
    }

    auto merged_written = write_if_changed(root_out_dir / build::Path("compile_commands.json"), wrap_compile_commands_array(merged));
    if (!merged_written) {
        return std::unexpected(merged_written.error());
    }
    return {};
}

} // namespace detail

inline auto main(int argc, char** argv, const Project& project) -> int {
    bool list_only = false;
    bool dump_graph = false;
    bool compile_commands = false;
    std::string platform_arg;
    std::string config_arg;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--list" || arg == "-l") {
            list_only = true;
        } else if (arg == "--dump-graph") {
            dump_graph = true;
        } else if (arg == "--compile-commands") {
            compile_commands = true;
        } else if ((arg == "--platform" || arg == "-p") && i + 1 < argc) {
            platform_arg = argv[++i];
        } else if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            config_arg = argv[++i];
        }
    }

    if (list_only) {
        print_summary(project, std::cout);
        return 0;
    }

    if (dump_graph) {
        auto dumped = Emitter{}.dump(project, std::cout);
        if (!dumped) {
            std::cerr << dumped.error().message << "\n";
            return 1;
        }
        return 0;
    }

    auto emitted = Emitter{}.emit(project);
    if (!emitted) {
        std::cerr << emitted.error().message << "\n";
        return 1;
    }

    if (compile_commands) {
        auto cc = detail::write_compile_commands(project, build::Path("_out"), platform_arg, config_arg);
        if (!cc) {
            std::cerr << cc.error().message << "\n";
            return 1;
        }
    }

    return 0;
}

} // namespace build::ir
