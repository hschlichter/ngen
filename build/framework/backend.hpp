#pragma once

#include "path.hpp"

namespace build {

class Platform;
class Configuration;

struct BuildVariant {
    const Platform* platform = nullptr;
    const Configuration* config = nullptr;
    Path out_dir;
};

} // namespace build
