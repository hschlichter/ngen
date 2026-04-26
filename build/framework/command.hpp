#pragma once

#include <string>
#include <vector>

namespace build {

struct Command {
    std::vector<std::string> argv;
};

} // namespace build
