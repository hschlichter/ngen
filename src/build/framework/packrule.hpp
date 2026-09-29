// build::PackRule and build::Pack — packing source assets into engine-ready packs.
//
// A **pack rule** says how one type of asset is packed: which asset ids it covers (glob patterns over project-relative
// paths, e.g. `shaders/*.vert`), which packer program packs them, the parameters the packer gets (fixed, or per
// configuration), and a version to bump when the output format changes. Rules are registered with the Project; the
// emitter resolves them per variant and writes them into the IR.
//
//     auto shaderRule = pack_rule("shader")
//                           .match({"shaders/*.vert", "shaders/*.frag"})
//                           .packer(shaderPacker)                       // a cxx::program target
//                           .param("optimize", per_config({{"debug", "0"}, {"release", "1"}}))
//                           .version(1);
//     p.pack_rule(shaderRule);
//
// A **pack target** lists assets to pack:
//
//     auto core = pack("core").assets(glob({.include = "shaders/*.vert"}));
//
// Each asset becomes one pack job edge running the first rule that matches it, writing the packed asset to
// `<out_dir>/packs/<asset id>`; the pack itself is a phony edge over its jobs. The asset id is the path as given,
// with forward slashes. An asset that no rule matches is an emit-time error.

#pragma once

#include "path.hpp"
#include "target.hpp"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace build {

// A parameter value: the same in every configuration, or one value per configuration name.
struct PackParamValue {
    std::string fixed;
    std::vector<std::pair<std::string, std::string>> per_config;

    auto resolve(const std::string& config) const -> std::string {
        for (const auto& [name, value] : per_config) {
            if (name == config) {
                return value;
            }
        }
        return fixed;
    }
};

inline auto per_config(std::initializer_list<std::pair<std::string, std::string>> values) -> PackParamValue {
    return PackParamValue{.fixed = {}, .per_config = values};
}

class PackRule {
public:
    explicit PackRule(std::string name) : name_(std::move(name)) {}

    auto match(std::vector<std::string> patterns) -> PackRule& {
        patterns_ = std::move(patterns);
        return *this;
    }

    auto packer(Target& program) -> PackRule& {
        packer_ = &program;
        return *this;
    }

    auto param(std::string key, std::string value) -> PackRule& {
        params_.emplace_back(std::move(key), PackParamValue{.fixed = std::move(value), .per_config = {}});
        return *this;
    }

    auto param(std::string key, PackParamValue value) -> PackRule& {
        params_.emplace_back(std::move(key), std::move(value));
        return *this;
    }

    auto version(std::uint32_t v) -> PackRule& {
        version_ = v;
        return *this;
    }

    auto name() const -> const std::string& { return name_; }
    auto patterns() const -> const std::vector<std::string>& { return patterns_; }
    auto packer_target() const -> Target* { return packer_; }
    auto params() const -> const std::vector<std::pair<std::string, PackParamValue>>& { return params_; }
    auto version_number() const -> std::uint32_t { return version_; }

private:
    std::string name_;
    std::vector<std::string> patterns_;
    Target* packer_ = nullptr;
    std::vector<std::pair<std::string, PackParamValue>> params_;
    std::uint32_t version_ = 1;
};

inline auto pack_rule(std::string name) -> PackRule {
    return PackRule(std::move(name));
}

// A target that packs a fixed list of assets into one pack file. Same wrapper pattern as `Phony`: owns its Target
// and attaches itself as the extension; copies re-attach.
class Pack {
public:
    explicit Pack(std::string name) : base_(std::make_shared<Target>(std::move(name))) { base_->extensions().attach(*this); }

    auto operator=(const Pack&) -> Pack& = delete;
    auto operator=(Pack&&) -> Pack& = delete;

    Pack(const Pack& other) : asset_paths(other.asset_paths), base_(other.base_) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    Pack(Pack&& other) noexcept : asset_paths(std::move(other.asset_paths)), base_(std::move(other.base_)) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    operator Target&() { return *base_; }
    operator const Target&() const { return *base_; }

    auto owner() -> Target& { return *base_; }
    auto owner() const -> const Target& { return *base_; }

    auto name() const -> const std::string& { return base_->name(); }

    auto assets(std::vector<Path> paths) -> Pack& {
        asset_paths = std::move(paths);
        return *this;
    }

    std::vector<Path> asset_paths;

private:
    std::shared_ptr<Target> base_;
};

inline auto pack(std::string name) -> Pack {
    return Pack(std::move(name));
}

} // namespace build
