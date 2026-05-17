// build::cxx::Configuration — the cxx-language extension on build::Configuration.
//
// Wraps a `build::Configuration` (held behind `std::shared_ptr`) and attaches itself as the cxx extension.
// Carries the per-config cxx values: `compile_flags`, `link_flags`, `defines`. Delegates `out_dir` to the base.
//
// Constructed via `cxx::configuration("debug")`. Registered with `Project::config()`. The IR emitter looks the
// wrapper up at emit time via `cxx::find_configuration(variant.config)` and layers its flags/defines on top of
// the platform values in the per-edge command.
//
// Same wrapper move/copy invariant as `cxx::Platform`: both constructors re-attach the back-pointer.

#pragma once

#include "../configuration.hpp"
#include "../path.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace build::cxx {

class Configuration {
public:
    explicit Configuration(std::string name) : base_(std::make_shared<build::Configuration>(std::move(name))) { base_->extensions().attach(*this); }

    auto operator=(const Configuration&) -> Configuration& = delete;
    auto operator=(Configuration&&) -> Configuration& = delete;

    Configuration(const Configuration& other)
        : defines_(other.defines_), compile_flags_(other.compile_flags_), link_flags_(other.link_flags_), base_(other.base_) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    Configuration(Configuration&& other) noexcept
        : defines_(std::move(other.defines_))
        , compile_flags_(std::move(other.compile_flags_))
        , link_flags_(std::move(other.link_flags_))
        , base_(std::move(other.base_)) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    operator build::Configuration&() { return *base_; }
    operator const build::Configuration&() const { return *base_; }

    auto owner() -> build::Configuration& { return *base_; }
    auto owner() const -> const build::Configuration& { return *base_; }

    auto name() const -> const std::string& { return base_->name(); }

    auto out_dir(Path value) -> Configuration& {
        base_->out_dir(std::move(value));
        return *this;
    }

    auto out_dir() const -> const Path& { return base_->out_dir(); }

    auto define(std::string value) -> Configuration& {
        defines_.push_back(std::move(value));
        return *this;
    }

    auto defines(std::vector<std::string> values) -> Configuration& {
        defines_.insert(defines_.end(), values.begin(), values.end());
        return *this;
    }

    auto compile_flag(std::string value) -> Configuration& {
        compile_flags_.push_back(std::move(value));
        return *this;
    }

    auto compile_flags(std::vector<std::string> values) -> Configuration& {
        compile_flags_.insert(compile_flags_.end(), values.begin(), values.end());
        return *this;
    }

    auto link_flag(std::string value) -> Configuration& {
        link_flags_.push_back(std::move(value));
        return *this;
    }

    auto link_flags(std::vector<std::string> values) -> Configuration& {
        link_flags_.insert(link_flags_.end(), values.begin(), values.end());
        return *this;
    }

    auto defines() const -> const std::vector<std::string>& { return defines_; }

    auto compile_flags() const -> const std::vector<std::string>& { return compile_flags_; }

    auto link_flags() const -> const std::vector<std::string>& { return link_flags_; }

private:
    std::vector<std::string> defines_;
    std::vector<std::string> compile_flags_;
    std::vector<std::string> link_flags_;
    std::shared_ptr<build::Configuration> base_;
};

inline auto configuration(std::string name) -> Configuration {
    return Configuration(std::move(name));
}

inline auto find_configuration(const build::Configuration& c) -> const Configuration* {
    return c.extensions().get<Configuration>();
}

} // namespace build::cxx
