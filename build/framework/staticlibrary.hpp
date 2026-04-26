#pragma once

#include "target.hpp"

namespace build {

class StaticLibrary final : public Target {
public:
    using Target::Target;
    std::string kind() const override { return "static_library"; }
};

} // namespace build
