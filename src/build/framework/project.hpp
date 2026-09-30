// build::Project — the registry of entry points for a single build run.
//
// Holds *pointers* (not ownership) to user-declared root targets, registered platforms, and registered
// configurations. The actual objects live in user code (typically `main()` locals in `build.cpp`); the Project
// just records the set the build should consider.
//
// `target` and `default_target` add roots; `platform` and `config` register variant axes. All registration calls
// are idempotent — registering the same reference twice is a no-op, so user code can re-register through
// multiple paths without bookkeeping.
//
// `build_all()` returns the transitive closure of every root in *post-order* (deps before dependents). This is
// what `ir::Emitter` walks: the post-order guarantees a child `cxx::ObjectFile`'s output already exists in the
// emitter's cache before its parent library's archive edge tries to look it up.

#pragma once

#include "configuration.hpp"
#include "platform.hpp"
#include "target.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace build {

class Project {
public:
    auto target(Target& t) -> void {
        if (std::find(roots_.begin(), roots_.end(), &t) == roots_.end()) {
            roots_.push_back(&t);
        }
    }

    auto default_target(Target& t) -> void {
        default_ = &t;
        target(t);
    }

    auto platform(Platform& p) -> void {
        if (std::find(platforms_.begin(), platforms_.end(), &p) == platforms_.end()) {
            platforms_.push_back(&p);
        }
    }

    auto config(Configuration& c) -> void {
        if (std::find(configs_.begin(), configs_.end(), &c) == configs_.end()) {
            configs_.push_back(&c);
        }
    }

    auto find_platform(std::string_view name) const -> Platform* {
        for (auto* p : platforms_) {
            if (p->name() == name) {
                return p;
            }
        }
        return nullptr;
    }

    auto find_config(std::string_view name) const -> Configuration* {
        for (auto* c : configs_) {
            if (c->name() == name) {
                return c;
            }
        }
        return nullptr;
    }

    auto find(std::string_view name) const -> Target* {
        for (auto* t : roots_) {
            if (t->name() == name) {
                return t;
            }
        }
        return nullptr;
    }

    auto build(std::string_view name) const -> std::vector<Target*> {
        auto* root = find(name);
        if (!root) {
            return {};
        }
        return reachable_from(*root);
    }

    auto build_all() const -> std::vector<Target*> {
        std::vector<Target*> result;
        std::unordered_set<Target*> seen;
        for (auto* root : roots_) {
            visit(root, seen, result);
        }
        return result;
    }

    auto default_build() const -> std::vector<Target*> {
        if (!default_) {
            return {};
        }
        return reachable_from(*default_);
    }

    auto default_target() const -> Target* { return default_; }

    auto roots() const -> const std::vector<Target*>& { return roots_; }

    auto platforms() const -> const std::vector<Platform*>& { return platforms_; }

    auto configs() const -> const std::vector<Configuration*>& { return configs_; }

private:
    static auto visit(Target* t, std::unordered_set<Target*>& seen, std::vector<Target*>& out) -> void {
        if (!t || seen.contains(t)) {
            return;
        }
        seen.insert(t);
        for (auto* dep : t->deps) {
            visit(dep, seen, out);
        }
        out.push_back(t);
    }

    static auto reachable_from(Target& root) -> std::vector<Target*> {
        std::vector<Target*> result;
        std::unordered_set<Target*> seen;
        visit(&root, seen, result);
        return result;
    }

    std::vector<Target*> roots_;
    Target* default_ = nullptr;
    std::vector<Platform*> platforms_;
    std::vector<Configuration*> configs_;
};

} // namespace build
