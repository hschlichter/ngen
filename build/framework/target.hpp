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

    const std::string& name() const { return name_; }
    virtual std::string kind() const = 0;

    Target& cxx(std::vector<Path> s) { sources = std::move(s); return *this; }
    Target& cxx(std::initializer_list<Path> s) { sources.assign(s.begin(), s.end()); return *this; }
    Target& std(std::string_view version) { cxx_std = std::string(version); return *this; }
    Target& define(std::string macro) { defines.push_back(std::move(macro)); return *this; }
    Target& include(Path dir) { private_includes.push_back(std::move(dir)); return *this; }
    Target& include(std::vector<Path> dirs) { private_includes.insert(private_includes.end(), dirs.begin(), dirs.end()); return *this; }
    Target& include(std::initializer_list<Path> dirs) { private_includes.insert(private_includes.end(), dirs.begin(), dirs.end()); return *this; }
    Target& public_include(Path dir) { public_includes.push_back(std::move(dir)); return *this; }
    Target& public_include(std::vector<Path> dirs) { public_includes.insert(public_includes.end(), dirs.begin(), dirs.end()); return *this; }
    Target& public_include(std::initializer_list<Path> dirs) { public_includes.insert(public_includes.end(), dirs.begin(), dirs.end()); return *this; }
    Target& warning_off(std::string_view name) { warning_suppressions.emplace_back(name); return *this; }
    Target& flag_raw(std::string token) { raw_compile_flags.push_back(std::move(token)); return *this; }
    Target& flags_raw(std::vector<std::string> tokens) { raw_compile_flags.insert(raw_compile_flags.end(), tokens.begin(), tokens.end()); return *this; }
    Target& optimize(OptLevel level) { opt = level; return *this; }
    Target& debug(bool enabled = true) { debug_info = enabled; return *this; }
    Target& pic(bool enabled = true) { needs_pic = enabled; return *this; }
    Target& depend_on(Target& other) { deps.push_back(&other); return *this; }
    Target& link(Target& other) { links.push_back(&other); return *this; }
    Target& link(std::string_view system_lib) { system_libs.emplace_back(system_lib); return *this; }
    Target& link_raw(std::string token) { raw_link_flags.push_back(std::move(token)); return *this; }
    Target& link_raw_many(std::vector<std::string> tokens) { raw_link_flags.insert(raw_link_flags.end(), tokens.begin(), tokens.end()); return *this; }
    Target& lib_search(Path dir) { lib_search_dirs.push_back(std::move(dir)); return *this; }
    Target& rpath(std::string path) { rpaths.push_back(std::move(path)); return *this; }
    Target& only_in(std::initializer_list<std::string_view> names) { for (auto n : names) only_configs_.emplace(n); return *this; }
    Target& except_in(std::initializer_list<std::string_view> names) { for (auto n : names) except_configs_.emplace(n); return *this; }
    Target& only_on(std::initializer_list<std::string_view> names) { for (auto n : names) only_platforms_.emplace(n); return *this; }
    Target& except_on(std::initializer_list<std::string_view> names) { for (auto n : names) except_platforms_.emplace(n); return *this; }

    bool enabled_for(std::string_view platform, std::string_view config) const {
        if (!only_platforms_.empty() && !only_platforms_.contains(std::string(platform))) return false;
        if (except_platforms_.contains(std::string(platform))) return false;
        if (!only_configs_.empty() && !only_configs_.contains(std::string(config))) return false;
        if (except_configs_.contains(std::string(config))) return false;
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
    std::string kind() const override { return "alias"; }

    Alias& to(Target& target) { fallback_ = &target; return *this; }
    Alias& select(std::string_view key, std::string_view value, Target& target) {
        selections_.emplace_back(key, value, &target);
        return *this;
    }
    Alias& fallback(Target& target) { fallback_ = &target; return *this; }

    Target* resolve(const std::map<std::string, std::string>& context) const {
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
