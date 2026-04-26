#pragma once

#include "target.hpp"

namespace build {

class Program final : public Target {
public:
    using Target::Target;
    std::string kind() const override { return "program"; }
};

} // namespace build
