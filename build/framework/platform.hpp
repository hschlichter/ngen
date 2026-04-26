#pragma once

#include "toolchain.hpp"

#include <memory>
#include <string>
#include <vector>

namespace build {

struct Platform {
    std::string name;
    std::string os;
    std::string graphics_api;
    std::unique_ptr<Toolchain> toolchain;
    std::vector<std::string> defines;
    std::vector<std::string> extra_cxx_flags;
    std::vector<std::string> extra_link_flags;
    std::vector<std::string> system_libs;
    std::string exe_suffix;
};

} // namespace build
