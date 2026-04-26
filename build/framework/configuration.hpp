#pragma once

#include "flags.hpp"
#include "path.hpp"

#include <string>
#include <vector>

namespace build {

struct Configuration {
    std::string name;
    OptLevel opt = OptLevel::O0;
    bool debug_info = true;
    Linkage default_linkage = Linkage::Static;
    std::vector<std::string> defines;
    std::vector<std::string> extra_cxx_flags;
    std::vector<std::string> extra_link_flags;
    Path out_dir = "_out";
};

} // namespace build
