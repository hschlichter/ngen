// build::Phony — a Target with no command and no outputs of its own: it exists to name a set of dependencies.
//
//   auto examples = phony("examples").depend_on(exampleTriangle).depend_on(exampleQuad);
//   p.target(examples);
//
// `ngen-build -p X -c Y examples` then builds every dependency. The emitter turns it into one phony edge
// (`kEdgeFlagPhony`) whose inputs are the dependencies' primary outputs and whose output is a virtual stamp
// under the variant's out dir, the same shape a `Tool` without outputs produces. Dependencies go through the
// ordinary `Target::depend_on`, so a Phony can depend on programs, libraries, tools, aliases or other phonies.
//
// Wrapper invariant: every move and copy constructor re-attaches `*this` on the base Target's `ExtensionMap`.

#pragma once

#include "target.hpp"

#include <memory>
#include <string>
#include <utility>

namespace build {

class Phony {
public:
    explicit Phony(std::string name) : base_(std::make_shared<Target>(std::move(name))) { base_->extensions().attach(*this); }

    auto operator=(const Phony&) -> Phony& = delete;
    auto operator=(Phony&&) -> Phony& = delete;

    Phony(const Phony& other) : base_(other.base_) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    Phony(Phony&& other) noexcept : base_(std::move(other.base_)) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    operator Target&() { return *base_; }
    operator const Target&() const { return *base_; }

    auto owner() -> Target& { return *base_; }
    auto owner() const -> const Target& { return *base_; }

    auto name() const -> const std::string& { return base_->name(); }

    auto depend_on(Target& target) -> Phony& {
        base_->depend_on(target);
        return *this;
    }

private:
    std::shared_ptr<Target> base_;
};

inline auto phony(std::string name) -> Phony {
    return Phony(std::move(name));
}

} // namespace build
