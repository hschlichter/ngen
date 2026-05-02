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

} // namespace build::cxx
