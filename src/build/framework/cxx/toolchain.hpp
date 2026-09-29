// build::cxx::Toolchain — the four cxx tools used at emit time.
//
// Just `compiler` / `archiver` / `linker` / `default_std` (plus `default_std = "c++23"` baked in). Composed
// *inside* `build::cxx::Platform` rather than registered as its own extension on `build::Platform`, because
// every platform has exactly one toolchain and the indirection would not pull its weight. Read by the IR
// emitter via `cxx_platform.toolchain()` when assembling per-edge `CompileInputs` / `LinkInputs`.
//
// Fluent setters return `Toolchain&` so the free factory `cxx::toolchain()` chains naturally.

#pragma once

#include <string>
#include <utility>

namespace build::cxx {

class Toolchain {
public:
    auto compiler(std::string value) -> Toolchain& {
        compiler_ = std::move(value);
        return *this;
    }

    auto archiver(std::string value) -> Toolchain& {
        archiver_ = std::move(value);
        return *this;
    }

    auto linker(std::string value) -> Toolchain& {
        linker_ = std::move(value);
        return *this;
    }

    auto default_std(std::string value) -> Toolchain& {
        default_std_ = std::move(value);
        return *this;
    }

    auto compiler() const -> const std::string& { return compiler_; }

    auto archiver() const -> const std::string& { return archiver_; }

    auto linker() const -> const std::string& { return linker_; }

    auto default_std() const -> const std::string& { return default_std_; }

private:
    std::string compiler_;
    std::string archiver_;
    std::string linker_;
    std::string default_std_ = "c++23";
};

inline auto toolchain() -> Toolchain {
    return Toolchain();
}

} // namespace build::cxx
