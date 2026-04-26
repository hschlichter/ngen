#pragma once

#include "configuration.hpp"
#include "platform.hpp"
#include "target.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace build {

class Graph {
public:
    template <class T, class... Args> auto add(std::string name, Args&&... args) -> T& {
        auto target = std::make_unique<T>(std::move(name), std::forward<Args>(args)...);
        T& ref = *target;
        targets_.push_back(std::move(target));
        return ref;
    }

    auto find(std::string_view name) const -> Target* {
        for (const auto& target : targets_) {
            if (target->name() == name) {
                return target.get();
            }
        }
        return nullptr;
    }

    auto addPlatform(Platform platform) -> void { platforms_.push_back(std::move(platform)); }
    auto addConfig(Configuration config) -> void { configs_.push_back(std::move(config)); }
    auto setDefault(Target& target) -> void { default_ = &target; }

    auto targets() const -> const std::vector<std::unique_ptr<Target>>& { return targets_; }
    auto platforms() const -> const std::vector<Platform>& { return platforms_; }
    auto configs() const -> const std::vector<Configuration>& { return configs_; }
    auto default_target() const -> Target* { return default_; }

private:
    std::vector<std::unique_ptr<Target>> targets_;
    std::vector<Platform> platforms_;
    std::vector<Configuration> configs_;
    Target* default_ = nullptr;
};

} // namespace build
