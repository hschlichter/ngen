// `print_summary` — render the project's platforms, configs, and top-level targets for `--list`.
//
// Called by `build.cpp` when the user passes `--list`, and also by `bootstrap.cpp` when the user invokes
// `ngen-build` without enough information to start a build. The output is the project's discovery surface:
// what platforms and configs are registered, and which targets you can actually ask for. Platform / config
// strings come straight from the Project's registration order.
//
// `list_roots` is the targets-only sub-helper, kept separate so a caller that only wants the target table
// (currently nobody) doesn't pay for platform/config printing.
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

inline auto print_summary(const Project& project, std::ostream& out) -> void {
    out << "Platforms:\n";
    for (const auto* p : project.platforms()) {
        out << "  " << p->name() << "\n";
    }

    out << "\nConfigurations:\n";
    for (const auto* c : project.configs()) {
        out << "  " << c->name() << "\n";
    }

    out << "\nTargets:\n";
    list_roots(project, out);
}

} // namespace build
