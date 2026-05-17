// build::Tool — a Target that runs an opaque shell command.
//
// Wraps a `build::Target` (held behind `std::shared_ptr`) and attaches as the Tool extension. Used for things
// that don't fit the cxx model: shader compilation (`glslc`), cleanup (`rm -rf $out_dir`), source-tree
// maintenance (`clang-format`, `clang-tidy`). The fluent API records the command template (`argv_template`),
// inputs, outputs, and a few flags; the IR emitter does the actual substitution and command-baking at emit time
// (see `ir/emit.hpp::emit_tool` / `emit_global_tool`).
//
// Three shapes the emitter recognises:
//   - **Outputs declared** — either explicitly (`outputs({...})`) or via the `for_each(inputs, fn)` form (one
//     output per input, output path computed by the user-supplied callback). Produces one edge per output plus
//     a phony aggregating stamp.
//   - **No outputs** — produces one stamp edge; the command runs whenever the stamp is rebuilt. `clean` uses
//     this shape with `$out_dir` substitution to nuke the variant tree.
//   - **`is_global`** — a single edge across all variants. The emitter still drops the edge into each variant's
//     IR for uniform addressability; `format` and `tidy` use this.
//
// `$in` / `$out` / `$out_dir` in `argv_template` are substituted at emit time.
//
// Same wrapper move/copy invariant as `Alias` — both constructors re-attach the extension back-pointer.

#pragma once

#include "path.hpp"
#include "target.hpp"
#include "variant.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace build {

class Tool {
public:
    using OutputFor = std::function<Path(const BuildVariant&, const Path&)>;

    explicit Tool(std::string name) : base_(std::make_shared<Target>(std::move(name))) { base_->extensions().attach(*this); }

    auto operator=(const Tool&) -> Tool& = delete;
    auto operator=(Tool&&) -> Tool& = delete;

    Tool(const Tool& other)
        : argv_template(other.argv_template)
        , tool_inputs(other.tool_inputs)
        , tool_outputs(other.tool_outputs)
        , output_for(other.output_for)
        , is_global(other.is_global)
        , base_(other.base_) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    Tool(Tool&& other) noexcept
        : argv_template(std::move(other.argv_template))
        , tool_inputs(std::move(other.tool_inputs))
        , tool_outputs(std::move(other.tool_outputs))
        , output_for(std::move(other.output_for))
        , is_global(other.is_global)
        , base_(std::move(other.base_)) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    operator Target&() { return *base_; }
    operator const Target&() const { return *base_; }

    auto owner() -> Target& { return *base_; }
    auto owner() const -> const Target& { return *base_; }

    auto name() const -> const std::string& { return base_->name(); }

    auto command(std::vector<std::string> argv) -> Tool& {
        argv_template = std::move(argv);
        return *this;
    }

    auto inputs(std::vector<Path> paths) -> Tool& {
        tool_inputs = std::move(paths);
        return *this;
    }

    auto outputs(std::vector<Path> paths) -> Tool& {
        tool_outputs = std::move(paths);
        return *this;
    }

    auto for_each(std::vector<Path> paths, OutputFor fn) -> Tool& {
        tool_inputs = std::move(paths);
        output_for = std::move(fn);
        return *this;
    }

    auto global(bool flag = true) -> Tool& {
        is_global = flag;
        return *this;
    }

    std::vector<std::string> argv_template;
    std::vector<Path> tool_inputs;
    std::vector<Path> tool_outputs;
    OutputFor output_for;
    bool is_global = false;

private:
    std::shared_ptr<Target> base_;
};

inline auto tool(std::string name) -> Tool {
    return Tool(std::move(name));
}

} // namespace build
