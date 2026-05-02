#pragma once

#include "configuration.hpp"
#include "platform.hpp"
#include "target.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
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

    auto platform(std::string name) -> Platform& {
        if (auto* existing = find_platform(name)) {
            return *existing;
        }
        platforms_.push_back(std::make_unique<Platform>(std::move(name)));
        return *platforms_.back();
    }

    auto config(std::string name) -> Configuration& {
        if (auto* existing = find_config(name)) {
            return *existing;
        }
        configs_.push_back(std::make_unique<Configuration>(std::move(name)));
        return *configs_.back();
    }

    auto find_platform(std::string_view name) -> Platform* {
        for (auto& p : platforms_) {
            if (p->name() == name) {
                return p.get();
            }
        }
        return nullptr;
    }

    auto find_config(std::string_view name) -> Configuration* {
        for (auto& c : configs_) {
            if (c->name() == name) {
                return c.get();
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

    auto platforms() const -> std::vector<Platform*> {
        std::vector<Platform*> out;
        out.reserve(platforms_.size());
        for (const auto& p : platforms_) {
            out.push_back(p.get());
        }
        return out;
    }

    auto configs() const -> std::vector<Configuration*> {
        std::vector<Configuration*> out;
        out.reserve(configs_.size());
        for (const auto& c : configs_) {
            out.push_back(c.get());
        }
        return out;
    }

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
    std::vector<std::unique_ptr<Platform>> platforms_;
    std::vector<std::unique_ptr<Configuration>> configs_;
};

} // namespace build
