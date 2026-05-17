// build::cxx::Target — the user-facing surface for cxx libraries and programs.
//
// Wraps a `build::Target` (held behind `std::shared_ptr`) and attaches itself as the cxx extension. Where the
// base `build::Target` carries only identity, deps, and gating, this wrapper carries everything a cxx library
// or program needs at emit time: a `Kind` (`StaticLibrary` / `SharedLibrary` / `Program`), the source list
// (materialised as `cxx::ObjectFile` children — see `objectfile.hpp`), per-target compile flags, defines,
// warning suppressions, the include search path (private `include` plus `public_include` which propagates to
// dependents), link inputs (other `cxx::Target`s, system libs, raw link flags, `lib_search` dirs, `rpath`s),
// and the C++ standard override.
//
// The fluent API is the only intended way to build one of these:
//
//     cxx::static_library("name")
//         .sources(glob({.include = "src/foo/**/*.cpp"}))
//         .include({"src/foo"})
//         .link(other_lib);
//
// Factory functions `cxx::static_library` / `cxx::shared_library` / `cxx::program` differ only in the `Kind`.
//
// `sources(...)` materialises one `cxx::ObjectFile` per `.cpp` and dep-edges it into this target's base.
// `for_source(path, fn)` looks the child up and calls `fn(ObjectFile&)` to apply per-TU overrides; missing
// source aborts (configuration error).
//
// `optimize(O3)` / `debug(true)` / `pic(true)` are sugar that append the right `-O3` / `-g` / `-fPIC` token to
// `compile_flags_data`; equivalent to the raw flag through `compile_flag("-O3")`.
//
// The public `*_data` fields exist for the IR emitter — it reads them directly when assembling per-edge inputs
// (see `build/ir/emit.hpp::emit_object_file` / `emit_cxx_library` / `emit_cxx_program`). User code mutates them
// through the fluent setters only.
//
// Same wrapper move/copy invariant as `cxx::Platform` / `Alias` / `Tool`: both constructors re-attach `*this`
// on the base's `ExtensionMap`.

#pragma once

#include "../path.hpp"
#include "../target.hpp"
#include "objectfile.hpp"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace build::cxx {

enum class Kind {
    StaticLibrary,
    SharedLibrary,
    Program,
};

enum class OptLevel {
    O0,
    O1,
    O2,
    O3,
};

class Target {
public:
    explicit Target(std::string name, Kind kind) : base_(std::make_shared<build::Target>(std::move(name))), kind_(kind) { base_->extensions().attach(*this); }

    auto operator=(const Target&) -> Target& = delete;
    auto operator=(Target&&) -> Target& = delete;

    Target(const Target& other)
        : objects_data(other.objects_data)
        , includes_data(other.includes_data)
        , public_includes_data(other.public_includes_data)
        , defines_data(other.defines_data)
        , warning_suppressions_data(other.warning_suppressions_data)
        , compile_flags_data(other.compile_flags_data)
        , std_data(other.std_data)
        , linked_targets_data(other.linked_targets_data)
        , system_libs_data(other.system_libs_data)
        , link_flags_data(other.link_flags_data)
        , lib_search_dirs_data(other.lib_search_dirs_data)
        , rpaths_data(other.rpaths_data)
        , base_(other.base_)
        , kind_(other.kind_) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    Target(Target&& other) noexcept
        : objects_data(std::move(other.objects_data))
        , includes_data(std::move(other.includes_data))
        , public_includes_data(std::move(other.public_includes_data))
        , defines_data(std::move(other.defines_data))
        , warning_suppressions_data(std::move(other.warning_suppressions_data))
        , compile_flags_data(std::move(other.compile_flags_data))
        , std_data(std::move(other.std_data))
        , linked_targets_data(std::move(other.linked_targets_data))
        , system_libs_data(std::move(other.system_libs_data))
        , link_flags_data(std::move(other.link_flags_data))
        , lib_search_dirs_data(std::move(other.lib_search_dirs_data))
        , rpaths_data(std::move(other.rpaths_data))
        , base_(std::move(other.base_))
        , kind_(other.kind_) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    operator build::Target&() { return *base_; }
    operator const build::Target&() const { return *base_; }

    auto owner() -> build::Target& { return *base_; }
    auto owner() const -> const build::Target& { return *base_; }

    auto kind() const -> Kind { return kind_; }

    auto sources(std::vector<Path> v) -> Target& {
        objects_data.reserve(objects_data.size() + v.size());
        for (auto& path : v) {
            objects_data.push_back(std::make_shared<ObjectFile>(base_, std::move(path)));
        }
        return *this;
    }
    auto sources(std::initializer_list<Path> v) -> Target& {
        objects_data.reserve(objects_data.size() + v.size());
        for (const auto& path : v) {
            objects_data.push_back(std::make_shared<ObjectFile>(base_, path));
        }
        return *this;
    }

    template <typename Fn>
    auto for_source(const Path& path, Fn&& fn) -> Target& {
        auto needle = path.string();
        for (auto& obj : objects_data) {
            if (obj->source().string() == needle) {
                std::forward<Fn>(fn)(*obj);
                return *this;
            }
        }
        std::cerr << "for_source: no source '" << needle << "' in target '" << base_->name() << "'\n";
        std::abort();
    }

    auto std(std::string_view v) -> Target& {
        std_data = std::string(v);
        return *this;
    }

    auto define(std::string macro) -> Target& {
        defines_data.push_back(std::move(macro));
        return *this;
    }

    auto defines(std::vector<std::string> values) -> Target& {
        defines_data.insert(defines_data.end(), values.begin(), values.end());
        return *this;
    }

    auto include(Path dir) -> Target& {
        includes_data.push_back(std::move(dir));
        return *this;
    }
    auto include(std::vector<Path> dirs) -> Target& {
        includes_data.insert(includes_data.end(), dirs.begin(), dirs.end());
        return *this;
    }
    auto include(std::initializer_list<Path> dirs) -> Target& {
        includes_data.insert(includes_data.end(), dirs.begin(), dirs.end());
        return *this;
    }

    auto public_include(Path dir) -> Target& {
        public_includes_data.push_back(std::move(dir));
        return *this;
    }
    auto public_include(std::vector<Path> dirs) -> Target& {
        public_includes_data.insert(public_includes_data.end(), dirs.begin(), dirs.end());
        return *this;
    }
    auto public_include(std::initializer_list<Path> dirs) -> Target& {
        public_includes_data.insert(public_includes_data.end(), dirs.begin(), dirs.end());
        return *this;
    }

    auto warning_off(std::string_view name) -> Target& {
        warning_suppressions_data.emplace_back(name);
        return *this;
    }

    auto compile_flag(std::string token) -> Target& {
        compile_flags_data.push_back(std::move(token));
        return *this;
    }

    auto compile_flags(std::vector<std::string> values) -> Target& {
        compile_flags_data.insert(compile_flags_data.end(), values.begin(), values.end());
        return *this;
    }

    auto optimize(OptLevel level) -> Target& {
        switch (level) {
            case OptLevel::O0:
                compile_flags_data.emplace_back("-O0");
                break;
            case OptLevel::O1:
                compile_flags_data.emplace_back("-O1");
                break;
            case OptLevel::O2:
                compile_flags_data.emplace_back("-O2");
                break;
            case OptLevel::O3:
                compile_flags_data.emplace_back("-O3");
                break;
        }
        return *this;
    }

    auto debug(bool enabled = true) -> Target& {
        if (enabled) {
            compile_flags_data.emplace_back("-g");
        }
        return *this;
    }

    auto pic(bool enabled = true) -> Target& {
        if (enabled) {
            compile_flags_data.emplace_back("-fPIC");
        }
        return *this;
    }

    auto only_on(std::initializer_list<std::string_view> names) -> Target& {
        base_->only_on(names);
        return *this;
    }

    auto except_on(std::initializer_list<std::string_view> names) -> Target& {
        base_->except_on(names);
        return *this;
    }

    auto only_in(std::initializer_list<std::string_view> names) -> Target& {
        base_->only_in(names);
        return *this;
    }

    auto except_in(std::initializer_list<std::string_view> names) -> Target& {
        base_->except_in(names);
        return *this;
    }

    auto depend_on(build::Target& other) -> Target& {
        base_->depend_on(other);
        return *this;
    }

    auto link(Target& other) -> Target& {
        base_->depend_on(other.owner());
        linked_targets_data.push_back(&other.owner());
        return *this;
    }

    auto link(build::Target& other) -> Target& {
        base_->depend_on(other);
        linked_targets_data.push_back(&other);
        return *this;
    }

    auto link(std::string_view system_lib) -> Target& {
        system_libs_data.emplace_back(system_lib);
        return *this;
    }

    auto system_libs(std::vector<std::string> values) -> Target& {
        system_libs_data.insert(system_libs_data.end(), values.begin(), values.end());
        return *this;
    }

    auto link_flag(std::string token) -> Target& {
        link_flags_data.push_back(std::move(token));
        return *this;
    }

    auto link_flags(std::vector<std::string> tokens) -> Target& {
        link_flags_data.insert(link_flags_data.end(), tokens.begin(), tokens.end());
        return *this;
    }

    auto lib_search(Path dir) -> Target& {
        lib_search_dirs_data.push_back(std::move(dir));
        return *this;
    }

    auto rpath(std::string p) -> Target& {
        rpaths_data.push_back(std::move(p));
        return *this;
    }

    // Public data, exposed for the IR emitter.
    // Mutated through the fluent API above; consumed by build::ir::detail::VariantEmitter.
    std::vector<std::shared_ptr<ObjectFile>> objects_data;
    std::vector<Path> includes_data;
    std::vector<Path> public_includes_data;
    std::vector<std::string> defines_data;
    std::vector<std::string> warning_suppressions_data;
    std::vector<std::string> compile_flags_data;
    std::string std_data;
    std::vector<build::Target*> linked_targets_data;
    std::vector<std::string> system_libs_data;
    std::vector<std::string> link_flags_data;
    std::vector<Path> lib_search_dirs_data;
    std::vector<std::string> rpaths_data;

private:
    std::shared_ptr<build::Target> base_;
    Kind kind_;
};

inline auto static_library(std::string name) -> Target {
    return Target(std::move(name), Kind::StaticLibrary);
}

inline auto shared_library(std::string name) -> Target {
    return Target(std::move(name), Kind::SharedLibrary);
}

inline auto program(std::string name) -> Target {
    return Target(std::move(name), Kind::Program);
}

} // namespace build::cxx
