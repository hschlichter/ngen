#pragma once

#include "extensionmap.hpp"
#include "path.hpp"

#include <string>
#include <utility>

namespace build {

class Configuration {
public:
    explicit Configuration(std::string name) : name_(std::move(name)) {}

    auto name() const -> const std::string& {
        return name_;
    }

    auto out_dir(Path value) -> Configuration& {
        out_dir_ = std::move(value);
        return *this;
    }

    auto out_dir() const -> const Path& {
        return out_dir_;
    }

    auto extensions() -> ExtensionMap& {
        return extensions_;
    }

    auto extensions() const -> const ExtensionMap& {
        return extensions_;
    }

private:
    std::string name_;
    Path out_dir_ = "_out";
    ExtensionMap extensions_;
};

} // namespace build
