#pragma once

#include "flags.hpp"
#include "target.hpp"

#include <optional>

namespace build {

class Library final : public Target {
public:
    using Target::Target;
    std::string kind() const override { return "library"; }
    Library& linkage(Linkage linkage) {
        forced_linkage = linkage;
        return *this;
    }

    std::optional<Linkage> forced_linkage;
};

} // namespace build
