#pragma once

#include "target.hpp"

#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace build {

class Alias final : public Target {
public:
    using Target::Target;

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
    Target* fallback_ = nullptr;
    std::vector<std::tuple<std::string, std::string, Target*>> selections_;
};

} // namespace build
