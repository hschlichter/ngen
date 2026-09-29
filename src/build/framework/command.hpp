// build::Command — an argv list.
//
// Carries a fully-tokenised shell argument vector for one edge: the program path followed by its arguments, no
// shell quoting yet. Constructed by the cxx command builders in `cxx/commands.hpp` (`compile_command` /
// `archive_command` / `link_command`) and by the `$in` / `$out` / `$out_dir` substitution in tool emission.
// Consumed by the IR emitter (`ir/emit.hpp::bake_command`), which shell-quotes each token and concatenates them
// into the baked command string stored on `ir::Edge::command`.
//
// Deliberately a dumb data carrier — it could be a `std::vector<std::string>` typedef. Keeping it a struct gives
// the type a name that shows up in builder signatures and makes intent obvious at the use sites.

#pragma once

#include <string>
#include <vector>

namespace build {

struct Command {
    std::vector<std::string> argv;
};

} // namespace build
