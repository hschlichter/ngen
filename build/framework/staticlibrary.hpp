#pragma once

#include "target.hpp"

namespace build {

class StaticLibrary final : public Target {
public:
    using Target::Target;
    auto kind() const -> std::string override { return "static_library"; }
};

} // namespace build
