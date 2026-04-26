#pragma once

#include "flags.hpp"
#include "path.hpp"

#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace build {

class Target {
public:
    explicit Target(std::string name) : name_(std::move(name)) {}
    virtual ~Target() = default;

    auto name() const -> const std::string& { return name_; }
    virtual auto kind() const -> std::string = 0;

    auto cxx(std::vector<Path> s) -> Target& {
        sources = std::move(s);
        return *this;
    }
    auto cxx(std::initializer_list<Path> s) -> Target& {
        sources.assign(s.begin(), s.end());
        return *this;
    }
    auto std(std::string_view version) -> Target& {
        cxx_std = std::string(version);
        return *this;
    }
    auto define(std::string macro) -> Target& {
        defines.push_back(std::move(macro));
        return *this;
    }
    auto include(Path dir) -> Target& {
        private_includes.push_back(std::move(dir));
        return *this;
    }
    auto include(std::vector<Path> dirs) -> Target& {
        private_includes.insert(private_includes.end(), dirs.begin(), dirs.end());
        return *this;
    }
    auto include(std::initializer_list<Path> dirs) -> Target& {
        private_includes.insert(private_includes.end(), dirs.begin(), dirs.end());
        return *this;
    }
    auto public_include(Path dir) -> Target& {
        public_includes.push_back(std::move(dir));
        return *this;
    }
    auto public_include(std::vector<Path> dirs) -> Target& {
        public_includes.insert(public_includes.end(), dirs.begin(), dirs.end());
        return *this;
    }
    auto public_include(std::initializer_list<Path> dirs) -> Target& {
        public_includes.insert(public_includes.end(), dirs.begin(), dirs.end());
        return *this;
    }
    auto warning_off(std::string_view name) -> Target& {
        warning_suppressions.emplace_back(name);
        return *this;
    }
    auto flag_raw(std::string token) -> Target& {
        raw_compile_flags.push_back(std::move(token));
        return *this;
    }
    auto flags_raw(std::vector<std::string> tokens) -> Target& {
        raw_compile_flags.insert(raw_compile_flags.end(), tokens.begin(), tokens.end());
        return *this;
    }
    auto optimize(OptLevel level) -> Target& {
        opt = level;
        return *this;
    }
    auto debug(bool enabled = true) -> Target& {
        debug_info = enabled;
        return *this;
    }
    auto pic(bool enabled = true) -> Target& {
        needs_pic = enabled;
        return *this;
    }
    auto depend_on(Target& other) -> Target& {
        deps.push_back(&other);
        return *this;
    }
    auto link(Target& other) -> Target& {
        links.push_back(&other);
        return *this;
    }
    auto link(std::string_view system_lib) -> Target& {
        system_libs.emplace_back(system_lib);
        return *this;
    }
    auto link_raw(std::string token) -> Target& {
        raw_link_flags.push_back(std::move(token));
        return *this;
    }
    auto link_raw_many(std::vector<std::string> tokens) -> Target& {
        raw_link_flags.insert(raw_link_flags.end(), tokens.begin(), tokens.end());
        return *this;
    }
    auto lib_search(Path dir) -> Target& {
        lib_search_dirs.push_back(std::move(dir));
        return *this;
    }
    auto rpath(std::string path) -> Target& {
        rpaths.push_back(std::move(path));
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

    std::vector<Path> sources;
    std::string cxx_std;
    std::vector<std::string> defines;
    std::vector<Path> private_includes;
    std::vector<Path> public_includes;
    std::vector<std::string> warning_suppressions;
    std::vector<std::string> raw_compile_flags;
    std::optional<OptLevel> opt;
    std::optional<bool> debug_info;
    bool needs_pic = true;
    std::vector<Target*> deps;
    std::vector<Target*> links;
    std::vector<std::string> system_libs;
    std::vector<std::string> raw_link_flags;
    std::vector<Path> lib_search_dirs;
    std::vector<std::string> rpaths;

private:
    std::string name_;
    std::set<std::string> only_configs_;
    std::set<std::string> except_configs_;
    std::set<std::string> only_platforms_;
    std::set<std::string> except_platforms_;
};

class Alias final : public Target {
public:
    using Target::Target;
    auto kind() const -> std::string override { return "alias"; }

    auto to(Target& target) -> Alias& {
        fallback_ = &target;
        return *this;
    }
    auto select(std::string_view key, std::string_view value, Target& target) -> Alias& {
        selections_.emplace_back(key, value, &target);
        return *this;
    }
    auto fallback(Target& target) -> Alias& {
        fallback_ = &target;
        return *this;
    }

    auto resolve(const std::map<std::string, std::string>& context) const -> Target* {
        for (const auto& [key, value, target] : selections_) {
            if (auto it = context.find(key); it != context.end() && it->second == value) {
                return target;
            }
        }
        return fallback_;
    }

private:
    Target* fallback_ = nullptr;
    std::vector<std::tuple<std::string, std::string, Target*>> selections_;
};

} // namespace build
