// build::Target — the node type for every build edge.
//
// One Target per "thing that has a name and a place in the dependency graph": libraries, programs, tools,
// aliases, phony stamps. The Target itself is language-agnostic — it carries only identity (`name`), dependency
// edges (`deps`), variant gating (`only_on` / `except_on` / `only_in` / `except_in`), and an `ExtensionMap`.
// Every language-specific concern (compile flags, includes, link list, source files) lives in an extension
// attached through that map. This is the seam that lets `build::cxx::Target` and a future `build::csharp::Target`
// coexist without changing this class.
//
// Targets are non-copyable, non-movable. Once a `Target*` is in hand, its address must stay valid for the rest
// of the build. The fluent wrapper types (`cxx::Target`, `Tool`, `Alias`) hold their `Target` behind
// `std::shared_ptr` and attach themselves to its `ExtensionMap` — that lets the wrapper move freely while the
// underlying node stays put.
//
// Constructed by user code in `build.cpp` via factory helpers (`cxx::static_library`, `tool`, `alias`). Walked
// by `ir::Emitter` (`build/ir/emit.hpp`) to produce build edges. `Project` does not own targets; user code does
// (typically `main()` locals), so the framework only ever borrows `Target*`.

#pragma once

#include "extensionmap.hpp"

#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace build {

class Target {
public:
    explicit Target(std::string name) : name_(std::move(name)) {}
    virtual ~Target() = default;

    Target(const Target&) = delete;
    Target(Target&&) = delete;
    auto operator=(const Target&) -> Target& = delete;
    auto operator=(Target&&) -> Target& = delete;

    auto name() const -> const std::string& { return name_; }

    auto depend_on(Target& other) -> Target& {
        deps.push_back(&other);
        return *this;
    }

    auto only_on(std::initializer_list<std::string_view> names) -> Target& {
        for (auto n : names) {
            only_platforms_.emplace(n);
        }
        return *this;
    }

    auto except_on(std::initializer_list<std::string_view> names) -> Target& {
        for (auto n : names) {
            except_platforms_.emplace(n);
        }
        return *this;
    }

    auto only_in(std::initializer_list<std::string_view> names) -> Target& {
        for (auto n : names) {
            only_configs_.emplace(n);
        }
        return *this;
    }

    auto except_in(std::initializer_list<std::string_view> names) -> Target& {
        for (auto n : names) {
            except_configs_.emplace(n);
        }
        return *this;
    }

    auto enabled_for(std::string_view platform, std::string_view config) const -> bool {
        if (!only_platforms_.empty() && !only_platforms_.contains(std::string(platform))) {
            return false;
        }
        if (except_platforms_.contains(std::string(platform))) {
            return false;
        }
        if (!only_configs_.empty() && !only_configs_.contains(std::string(config))) {
            return false;
        }
        if (except_configs_.contains(std::string(config))) {
            return false;
        }
        return true;
    }

    auto extensions() -> ExtensionMap& { return extensions_; }

    auto extensions() const -> const ExtensionMap& { return extensions_; }

    template <typename Ext>
    auto register_extension(Ext& ext) -> void { extensions_.attach(ext); }

    template <typename Ext>
    auto extension() const -> Ext* {
        return const_cast<Ext*>(extensions_.get<Ext>());
    }

    template <typename Ext>
    auto has_extension() const -> bool { return extensions_.has<Ext>(); }

    std::vector<Target*> deps;

private:
    std::string name_;
    std::set<std::string> only_platforms_;
    std::set<std::string> except_platforms_;
    std::set<std::string> only_configs_;
    std::set<std::string> except_configs_;
    ExtensionMap extensions_;
};

} // namespace build
