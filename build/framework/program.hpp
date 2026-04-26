#pragma once

#include "target.hpp"

namespace build {

class Program final : public Target {
public:
    using Target::Target;
    auto kind() const -> std::string override { return "program"; }
};

} // namespace build
