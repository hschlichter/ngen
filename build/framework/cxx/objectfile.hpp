// build::cxx::ObjectFile — one node per translation unit.
//
// Every `.cpp` declared via `cxx::Target::sources(...)` becomes one `ObjectFile`. Each owns its own
// `build::Target` (so it shows up in the dependency graph), holds a `shared_ptr` back to its parent's *base*
// (not to the parent wrapper, which is a value type that gets moved during the fluent builder dance), and
// attaches itself as the `cxx::ObjectFile` extension on its own base. The parent's base dep-edges into this
// object's base, so the IR emitter walks ObjectFiles via the normal post-order traversal.
//
// Naming: `<parent-name>/<source-path>`, e.g. `renderer/src/renderer/foo.cpp`. Composite by design — uniqueness
// is guaranteed even when the same source is compiled into two libraries (each gets its own ObjectFile with
// its own output path).
//
// `parent() -> cxx::Target*` looks the live wrapper up through the parent base's `ExtensionMap`. The
// indirection keeps `parent()` correct even after `cxx::Target` has been copied/moved into its final home.
//
// Non-copyable, non-movable — the `shared_ptr` is what moves between `objects_data` slots, never the object.
//
// Per-TU overrides (`defines_data`, `warning_suppressions_data`, `compile_flags_data`, `std_data`) start empty
// and are layered on top in the order (platform → config → parent → object) at emit time. See
// `ir/emit.hpp::emit_object_file`.

#pragma once

#include "../path.hpp"
#include "../target.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace build::cxx {

class Target;

class ObjectFile {
public:
    ObjectFile(std::shared_ptr<build::Target> parent_base, Path source)
        : base_(std::make_shared<build::Target>(parent_base->name() + "/" + source.string()))
        , parent_base_(std::move(parent_base))
        , source_(std::move(source)) {
        base_->extensions().attach(*this);
        parent_base_->depend_on(*base_);
    }

    ObjectFile(const ObjectFile&) = delete;
    ObjectFile(ObjectFile&&) = delete;
    auto operator=(const ObjectFile&) -> ObjectFile& = delete;
    auto operator=(ObjectFile&&) -> ObjectFile& = delete;
    ~ObjectFile() = default;

    auto owner() -> build::Target& { return *base_; }
    auto owner() const -> const build::Target& { return *base_; }

    auto source() const -> const Path& { return source_; }

    auto parent() const -> const Target* { return parent_base_->extensions().get<Target>(); }
    auto parent() -> Target* { return parent_base_->extensions().get<Target>(); }

    auto define(std::string macro) -> ObjectFile& {
        defines_data.push_back(std::move(macro));
        return *this;
    }

    auto defines(std::vector<std::string> values) -> ObjectFile& {
        defines_data.insert(defines_data.end(), values.begin(), values.end());
        return *this;
    }

    auto warning_off(std::string_view name) -> ObjectFile& {
        warning_suppressions_data.emplace_back(name);
        return *this;
    }

    auto compile_flag(std::string token) -> ObjectFile& {
        compile_flags_data.push_back(std::move(token));
        return *this;
    }

    auto compile_flags(std::vector<std::string> values) -> ObjectFile& {
        compile_flags_data.insert(compile_flags_data.end(), values.begin(), values.end());
        return *this;
    }

    auto std(std::string_view value) -> ObjectFile& {
        std_data = std::string(value);
        return *this;
    }

    std::vector<std::string> defines_data;
    std::vector<std::string> warning_suppressions_data;
    std::vector<std::string> compile_flags_data;
    std::string std_data;

private:
    std::shared_ptr<build::Target> base_;
    std::shared_ptr<build::Target> parent_base_;
    Path source_;
};

} // namespace build::cxx
