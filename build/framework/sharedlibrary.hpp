#pragma once

#include "target.hpp"

namespace build {

class SharedLibrary final : public Target {
public:
    using Target::Target;
    std::string kind() const override { return "shared_library"; }
};

} // namespace build
