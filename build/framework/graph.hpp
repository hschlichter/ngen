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
    template <class T, class... Args> T& add(std::string name, Args&&... args) {
        auto target = std::make_unique<T>(std::move(name), std::forward<Args>(args)...);
        T& ref = *target;
        targets_.push_back(std::move(target));
        return ref;
    }

    Target* find(std::string_view name) const {
        for (const auto& target : targets_) {
            if (target->name() == name) {
                return target.get();
            }
        }
        return nullptr;
    }

    void addPlatform(Platform platform) { platforms_.push_back(std::move(platform)); }
    void addConfig(Configuration config) { configs_.push_back(std::move(config)); }
    void setDefault(Target& target) { default_ = &target; }

    const std::vector<std::unique_ptr<Target>>& targets() const { return targets_; }
    const std::vector<Platform>& platforms() const { return platforms_; }
    const std::vector<Configuration>& configs() const { return configs_; }
    Target* default_target() const { return default_; }

private:
    std::vector<std::unique_ptr<Target>> targets_;
    std::vector<Platform> platforms_;
    std::vector<Configuration> configs_;
    Target* default_ = nullptr;
};

} // namespace build
