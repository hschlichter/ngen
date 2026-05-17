// build::Alias — a Target whose meaning depends on the build variant.
//
// Wraps a `build::Target` (stored behind `std::shared_ptr`) and attaches itself as the Alias extension on that
// target's `ExtensionMap`. The list of `select(key, value, target)` rules and an optional `fallback` together
// describe how to resolve to a concrete target given a `(platform, config)` context. `resolve(context)` walks
// the rules in registration order and returns the first match, falling back if none match.
//
// Used for graph-level indirection: `rhi-backend` is an Alias that resolves to `rhivulkan` on `linux-vulkan` and
// would resolve to some other backend on a future platform. Resolution happens inside the IR emitter via
// `detail::resolve_alias`, which walks through any chain of Aliases until a non-Alias target is reached.
//
// Wrapper invariant: every move and copy constructor re-attaches `*this` on the base Target's `ExtensionMap`.
// This keeps the extension back-pointer pointed at the live wrapper even when user code materialises the Alias
// from a temporary via the fluent factory `alias(name).select(...)`.

#pragma once

#include "target.hpp"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace build {

class Alias {
public:
    explicit Alias(std::string name) : base_(std::make_shared<Target>(std::move(name))) { base_->extensions().attach(*this); }

    auto operator=(const Alias&) -> Alias& = delete;
    auto operator=(Alias&&) -> Alias& = delete;

    Alias(const Alias& other) : selections_(other.selections_), fallback_(other.fallback_), base_(other.base_) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    Alias(Alias&& other) noexcept : selections_(std::move(other.selections_)), fallback_(other.fallback_), base_(std::move(other.base_)) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    operator Target&() { return *base_; }
    operator const Target&() const { return *base_; }

    auto owner() -> Target& { return *base_; }
    auto owner() const -> const Target& { return *base_; }

    auto name() const -> const std::string& { return base_->name(); }

    auto to(Target& target) -> Alias& {
        fallback_ = &target;
        return *this;
    }

    auto select(std::string_view key, std::string_view value, Target& target) -> Alias& {
        selections_.emplace_back(key, value, &target);
        return *this;
    }

    auto fallback(Target& target) -> Alias& {
        fallback_ = &target;
        return *this;
    }

    auto resolve(const std::map<std::string, std::string>& context) const -> Target* {
        for (const auto& [key, value, target] : selections_) {
            if (auto it = context.find(key); it != context.end() && it->second == value) {
                return target;
            }
        }
        return fallback_;
    }

private:
    std::vector<std::tuple<std::string, std::string, Target*>> selections_;
    Target* fallback_ = nullptr;
    std::shared_ptr<Target> base_;
};

inline auto alias(std::string name) -> Alias {
    return Alias(std::move(name));
}

} // namespace build
