#pragma once

#include "../platform.hpp"
#include "toolchain.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace build::cxx {

class Platform {
public:
    explicit Platform(std::string name) : base_(std::make_shared<build::Platform>(std::move(name))) { base_->extensions().attach(*this); }

    auto operator=(const Platform&) -> Platform& = delete;
    auto operator=(Platform&&) -> Platform& = delete;

    Platform(const Platform& other)
        : toolchain_(other.toolchain_)
        , defines_(other.defines_)
        , compile_flags_(other.compile_flags_)
        , link_flags_(other.link_flags_)
        , system_libs_(other.system_libs_)
        , base_(other.base_) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    Platform(Platform&& other) noexcept
        : toolchain_(std::move(other.toolchain_))
        , defines_(std::move(other.defines_))
        , compile_flags_(std::move(other.compile_flags_))
        , link_flags_(std::move(other.link_flags_))
        , system_libs_(std::move(other.system_libs_))
        , base_(std::move(other.base_)) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    operator build::Platform&() { return *base_; }
    operator const build::Platform&() const { return *base_; }

    auto owner() -> build::Platform& { return *base_; }
    auto owner() const -> const build::Platform& { return *base_; }

    auto name() const -> const std::string& { return base_->name(); }

    auto os(std::string value) -> Platform& {
        base_->os(std::move(value));
        return *this;
    }

    auto graphics_api(std::string value) -> Platform& {
        base_->graphics_api(std::move(value));
        return *this;
    }

    auto exe_suffix(std::string value) -> Platform& {
        base_->exe_suffix(std::move(value));
        return *this;
    }

    auto os() const -> const std::string& { return base_->os(); }

    auto graphics_api() const -> const std::string& { return base_->graphics_api(); }

    auto exe_suffix() const -> const std::string& { return base_->exe_suffix(); }

    auto toolchain() -> Toolchain& { return toolchain_; }

    auto toolchain() const -> const Toolchain& { return toolchain_; }

    auto define(std::string value) -> Platform& {
        defines_.push_back(std::move(value));
        return *this;
    }

    auto defines(std::vector<std::string> values) -> Platform& {
        defines_.insert(defines_.end(), values.begin(), values.end());
        return *this;
    }

    auto compile_flag(std::string value) -> Platform& {
        compile_flags_.push_back(std::move(value));
        return *this;
    }

    auto compile_flags(std::vector<std::string> values) -> Platform& {
        compile_flags_.insert(compile_flags_.end(), values.begin(), values.end());
        return *this;
    }

    auto link_flag(std::string value) -> Platform& {
        link_flags_.push_back(std::move(value));
        return *this;
    }

    auto link_flags(std::vector<std::string> values) -> Platform& {
        link_flags_.insert(link_flags_.end(), values.begin(), values.end());
        return *this;
    }

    auto system_lib(std::string value) -> Platform& {
        system_libs_.push_back(std::move(value));
        return *this;
    }

    auto system_libs(std::vector<std::string> values) -> Platform& {
        system_libs_.insert(system_libs_.end(), values.begin(), values.end());
        return *this;
    }

    auto defines() const -> const std::vector<std::string>& { return defines_; }

    auto compile_flags() const -> const std::vector<std::string>& { return compile_flags_; }

    auto link_flags() const -> const std::vector<std::string>& { return link_flags_; }

    auto system_libs() const -> const std::vector<std::string>& { return system_libs_; }

private:
    Toolchain toolchain_;
    std::vector<std::string> defines_;
    std::vector<std::string> compile_flags_;
    std::vector<std::string> link_flags_;
    std::vector<std::string> system_libs_;
    std::shared_ptr<build::Platform> base_;
};

inline auto platform(std::string name) -> Platform {
    return Platform(std::move(name));
}

inline auto find_platform(const build::Platform& p) -> const Platform* {
    if (!p.extensions().has<Platform>()) {
        return nullptr;
    }
    return &p.extensions().get<Platform>();
}

} // namespace build::cxx
