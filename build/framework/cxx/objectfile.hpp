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

// One translation unit, addressable as its own build::Target node.
//
// Lifetime: held by shared_ptr in the parent cxx::Target's objects_data.
// The parent's build::Target depends on this object's build::Target, so the
// emitter visits it as part of the normal dep walk.
//
// parent_base_ holds the parent's shared base (not the cxx::Target wrapper),
// because cxx::Target itself is a value type that gets copied/moved into
// place. The cxx extension on parent_base_ always points to the live
// cxx::Target instance via its ExtensionMap.
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

    // Returns the live parent cxx::Target. The ExtensionMap re-binds the
    // back-pointer when the parent value gets copied/moved into its final
    // home, so this stays correct across the fluent-builder dance.
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

    // Per-TU overrides — empty by default. Merged on top of parent values
    // at emit time (platform → config → parent → ObjectFile).
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
