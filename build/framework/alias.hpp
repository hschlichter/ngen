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

    Alias(const Alias& other)
        : selections_(other.selections_)
        , fallback_(other.fallback_)
        , base_(other.base_) {
        if (base_) {
            base_->extensions().attach(*this);
        }
    }

    Alias(Alias&& other) noexcept
        : selections_(std::move(other.selections_))
        , fallback_(other.fallback_)
        , base_(std::move(other.base_)) {
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
