#pragma once

#include "command.hpp"
#include "flags.hpp"
#include "path.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace build {

struct CompileIntent {
    Path source;
    Path object;
    std::string std;
    OptLevel opt = OptLevel::O0;
    bool debug = true;
    bool pic = true;
    std::vector<std::string> defines;
    std::vector<Path> includes;
    std::vector<std::string> warning_off;
    std::vector<std::string> raw;
};

struct LinkIntent {
    std::vector<Path> objects;
    std::vector<Path> archives;
    std::vector<Path> shared_libs;
    std::vector<std::string> external_libs;
    std::vector<Path> lib_search;
    std::vector<std::string> rpaths;
    std::vector<std::string> raw;
    Path output;
};

class Toolchain {
public:
    struct DepSupport {
        std::string depfile;
        std::string deps_format;
    };

    virtual ~Toolchain() = default;
    virtual std::string name() const = 0;
    virtual Command compile_cxx(const CompileIntent& intent) const = 0;
    virtual Command archive(std::vector<Path> objects, Path output) const = 0;
    virtual Command link_exe(const LinkIntent& intent) const = 0;
    virtual Command link_shared(const LinkIntent& intent) const = 0;
    virtual std::optional<DepSupport> dep_support(Path object) const = 0;
    virtual std::string static_lib_name(std::string_view stem) const = 0;
    virtual std::string shared_lib_name(std::string_view stem) const = 0;
    virtual std::string exe_name(std::string_view stem, std::string_view platform_suffix) const = 0;
};

} // namespace build
