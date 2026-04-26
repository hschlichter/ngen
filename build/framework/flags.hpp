#pragma once

#include <string>

namespace build {

enum class OptLevel { O0, O1, O2, O3 };
enum class Linkage { Static, Shared };

struct Error {
    std::string message;
};

} // namespace build
