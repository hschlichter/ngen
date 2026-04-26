#pragma once

#include "flags.hpp"
#include "target.hpp"

#include <optional>

namespace build {

class Library final : public Target {
public:
    using Target::Target;
    auto kind() const -> std::string override { return "library"; }
    auto linkage(Linkage linkage) -> Library& {
        forced_linkage = linkage;
        return *this;
    }

    std::optional<Linkage> forced_linkage;
};

} // namespace build
