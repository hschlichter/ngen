// build::Configuration — language-agnostic build-configuration identity.
//
// Symmetric to `build::Platform`: carries only the identity (`name`) and an output root (`out_dir`, defaulting
// to `_out`) plus an `ExtensionMap`. Per-config compile flags, link flags, and defines live in
// `build::cxx::Configuration`, attached here as the cxx extension.
//
// Constructed by user code via `cxx::configuration("debug")`. Registered with `Project::config()`. Read at emit
// time via `BuildVariant::config`; `out_dir` participates in `variant.out_dir = root / platform / config`.

#pragma once

#include "extensionmap.hpp"
#include "path.hpp"

#include <string>
#include <utility>

namespace build {

class Configuration {
public:
    explicit Configuration(std::string name) : name_(std::move(name)) {}

    auto name() const -> const std::string& { return name_; }

    auto out_dir(Path value) -> Configuration& {
        out_dir_ = std::move(value);
        return *this;
    }

    auto out_dir() const -> const Path& { return out_dir_; }

    auto extensions() -> ExtensionMap& { return extensions_; }

    auto extensions() const -> const ExtensionMap& { return extensions_; }

private:
    std::string name_;
    Path out_dir_ = "_out";
    ExtensionMap extensions_;
};

} // namespace build
