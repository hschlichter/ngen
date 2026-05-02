#pragma once

#include "../configuration.hpp"

#include <string>
#include <utility>
#include <vector>

namespace build::cxx {

class Configuration {
public:
    auto define(std::string value) -> Configuration& {
        defines_.push_back(std::move(value));
        return *this;
    }

    auto compile_flag(std::string value) -> Configuration& {
        compile_flags_.push_back(std::move(value));
        return *this;
    }

    auto link_flag(std::string value) -> Configuration& {
        link_flags_.push_back(std::move(value));
        return *this;
    }

    auto defines() const -> const std::vector<std::string>& {
        return defines_;
    }

    auto compile_flags() const -> const std::vector<std::string>& {
        return compile_flags_;
    }

    auto link_flags() const -> const std::vector<std::string>& {
        return link_flags_;
    }

private:
    std::vector<std::string> defines_;
    std::vector<std::string> compile_flags_;
    std::vector<std::string> link_flags_;
};

inline auto configuration(build::Configuration& c) -> Configuration& {
    if (!c.extensions().has<Configuration>()) {
        return c.extensions().add<Configuration>();
    }
    return c.extensions().get<Configuration>();
}

inline auto find_configuration(const build::Configuration& c) -> const Configuration* {
    if (!c.extensions().has<Configuration>()) {
        return nullptr;
    }
    return &c.extensions().get<Configuration>();
}

} // namespace build::cxx
