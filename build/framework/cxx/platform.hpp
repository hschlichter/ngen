#pragma once

#include "../platform.hpp"
#include "toolchain.hpp"

#include <string>
#include <utility>
#include <vector>

namespace build::cxx {

class Platform {
public:
    auto toolchain() -> Toolchain& { return toolchain_; }

    auto toolchain() const -> const Toolchain& { return toolchain_; }

    auto define(std::string value) -> Platform& {
        defines_.push_back(std::move(value));
        return *this;
    }

    auto compile_flag(std::string value) -> Platform& {
        compile_flags_.push_back(std::move(value));
        return *this;
    }

    auto link_flag(std::string value) -> Platform& {
        link_flags_.push_back(std::move(value));
        return *this;
    }

    auto system_lib(std::string value) -> Platform& {
        system_libs_.push_back(std::move(value));
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
};

inline auto platform(build::Platform& p) -> Platform& {
    if (!p.extensions().has<Platform>()) {
        return p.extensions().add<Platform>();
    }
    return p.extensions().get<Platform>();
}

inline auto find_platform(const build::Platform& p) -> const Platform* {
    if (!p.extensions().has<Platform>()) {
        return nullptr;
    }
    return &p.extensions().get<Platform>();
}

} // namespace build::cxx
