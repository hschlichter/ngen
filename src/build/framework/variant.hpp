// build::BuildVariant — identity tuple for "which build is this".
//
// One `(platform, config, out_dir)` value flows through the entire IR emit pass. The emitter constructs one per
// (platform, config) cross product and passes it to every recursive helper (`emit_target`, `collect_includes`,
// `object_path`, etc.) and to `Tool::output_for` callbacks for per-input output-path computation. The two pointers
// are borrowed — variant values are cheap to copy and never own their referents.
//
// The forward declarations of `Platform` and `Configuration` here let the struct flow through code that only
// stores or passes it; consumers that dereference the pointers (`variant.platform->name()`, etc.) include the
// full headers themselves.

#pragma once

#include "path.hpp"

namespace build {

class Platform;
class Configuration;

struct BuildVariant {
    const Platform* platform = nullptr;
    const Configuration* config = nullptr;
    Path out_dir;
};

} // namespace build
