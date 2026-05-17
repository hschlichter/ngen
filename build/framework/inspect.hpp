// `list_roots` — render the project's top-level targets for `--list`.
//
// Single helper, called by `build/build.cpp` when the user passes `--list`. Walks `Project::roots()`, classifies
// each by which extension it carries (`cxx::Target` kind, `Tool`, `Alias`, or bare phony), and prints a
// column-aligned `name  [kind]  (default)?` line.
//
// Lives in its own file because nothing else in the framework needs `--list`, and pulling `cxx/target.hpp` into
// `project.hpp` (just to describe the kind) would add a hot include for every consumer of `Project`.

#pragma once

#include "alias.hpp"
#include "cxx/target.hpp"
#include "project.hpp"
#include "target.hpp"
#include "tool.hpp"

#include <ostream>
#include <string_view>

namespace build {

inline auto describe_kind(const Target& t) -> std::string_view {
    if (const auto* cx = t.extension<cxx::Target>()) {
        switch (cx->kind()) {
            case cxx::Kind::Program:
                return "program";
            case cxx::Kind::StaticLibrary:
                return "static-library";
            case cxx::Kind::SharedLibrary:
                return "shared-library";
        }
    }

    if (t.has_extension<Tool>()) {
        return "tool";
    }

    if (t.has_extension<Alias>()) {
        return "alias";
    }

    return "phony";
}

inline auto list_roots(const Project& project, std::ostream& out) -> void {
    const auto* default_target = project.default_target();
    std::size_t name_width = 0;
    for (const auto* t : project.roots()) {
        name_width = std::max(name_width, t->name().size());
    }

    for (const auto* t : project.roots()) {
        out << "  " << t->name();
        for (std::size_t i = t->name().size(); i < name_width; ++i) {
            out << ' ';
        }

        out << "  [" << describe_kind(*t) << "]";
        if (t == default_target) {
            out << "  (default)";
        }

        out << '\n';
    }
}

} // namespace build
