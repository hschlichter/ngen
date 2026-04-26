#pragma once

#include "target.hpp"

namespace build {

class SharedLibrary final : public Target {
public:
    using Target::Target;
    auto kind() const -> std::string override { return "shared_library"; }
};

} // namespace build
