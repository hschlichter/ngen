#pragma once

#include <string>

namespace build::cxx {

enum class OptLevel {
    O0,
    O1,
    O2,
    O3,
};

inline auto opt_flag(OptLevel opt) -> std::string {
    switch (opt) {
        case OptLevel::O0:
            return "-O0";
        case OptLevel::O1:
            return "-O1";
        case OptLevel::O2:
            return "-O2";
        case OptLevel::O3:
            return "-O3";
    }
    return "-O0";
}

} // namespace build::cxx
