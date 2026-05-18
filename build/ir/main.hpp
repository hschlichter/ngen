// build::ir::main — the graph-stage `main()` body, parameterised over the project.
//
// `build.cpp` constructs the user's `Project` and ends with `return ir::main(argc, argv, p);`. Everything in
// this file is project-agnostic CLI plumbing: parse argv, dispatch to `print_summary` for `--list`, to
// `Emitter::dump` for `--dump-graph`, or to `Emitter::emit` otherwise.
//
// Lives in `ir/` because it's the entry point for the IR-emitter binary (`ngen-build-graph`). Sits at the same
// layer as the `ngen::run::execute()` entry point for the runner.

#pragma once

#include "../framework/inspect.hpp"
#include "../framework/project.hpp"
#include "emit.hpp"

#include <iostream>
#include <string_view>

namespace build::ir {

inline auto main(int argc, char** argv, const Project& project) -> int {
    bool list_only = false;
    bool dump_graph = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--list" || arg == "-l") {
            list_only = true;
        } else if (arg == "--dump-graph") {
            dump_graph = true;
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
    return 0;
}

} // namespace build::ir
