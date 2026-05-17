// build::Platform — language-agnostic platform identity.
//
// Mirrors the framework / cxx split seen on `Target`: this base carries only what isn't language-specific —
// `name`, `os`, `graphics_api`, `exe_suffix` — plus an `ExtensionMap`. The cxx-side payload (compile flags,
// link flags, defines, system libs, toolchain) lives in `build::cxx::Platform`, attached as an extension here.
//
// Constructed by user code via the fluent `cxx::platform("my-platform")` (which wraps a `build::Platform`
// behind a `shared_ptr` and attaches itself). Registered with `Project::platform()`. Read at emit time via
// `BuildVariant::platform`.

#pragma once

#include "extensionmap.hpp"

#include <string>
#include <utility>

namespace build {

class Platform {
public:
    explicit Platform(std::string name) : name_(std::move(name)) {}

    auto name() const -> const std::string& { return name_; }

    auto os(std::string value) -> Platform& {
        os_ = std::move(value);
        return *this;
    }

    auto graphics_api(std::string value) -> Platform& {
        graphics_api_ = std::move(value);
        return *this;
    }

    auto exe_suffix(std::string value) -> Platform& {
        exe_suffix_ = std::move(value);
        return *this;
    }

    auto os() const -> const std::string& { return os_; }

    auto graphics_api() const -> const std::string& { return graphics_api_; }

    auto exe_suffix() const -> const std::string& { return exe_suffix_; }

    auto extensions() -> ExtensionMap& { return extensions_; }

    auto extensions() const -> const ExtensionMap& { return extensions_; }

private:
    std::string name_;
    std::string os_;
    std::string graphics_api_;
    std::string exe_suffix_;
    ExtensionMap extensions_;
};

} // namespace build
