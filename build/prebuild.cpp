#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <cstdlib>

namespace {

struct Error {
    std::string message;
};

std::expected<void, Error> write_if_changed(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return std::unexpected(Error{"failed to create directory " + path.parent_path().string() + ": " + ec.message()});
    }
    std::ifstream in(path);
    std::ostringstream current;
    current << in.rdbuf();
    if (in && current.str() == text) {
        return {};
    }
    std::ofstream out(path);
    if (!out) {
        return std::unexpected(Error{"failed to open " + path.string() + " for writing"});
    }
    out << text;
    if (!out) {
        return std::unexpected(Error{"failed to write " + path.string()});
    }
    return {};
}

} // namespace

int main() {
    auto written = write_if_changed("_out/ngen-build-graph.ninja", R"(cxx = clang++
cxxflags = -std=c++23 -O0 -g -Wall -Wextra -Ibuild/framework
builddir = _out/.ninja

rule cxx
  command = mkdir -p _out && $cxx $cxxflags -o $out build/build.cpp
  description = GRAPH $out

build _out/ngen-build-graph: cxx build/build.cpp $
    build/framework/backend.hpp $
    build/framework/backendninja.hpp $
    build/framework/command.hpp $
    build/framework/configuration.hpp $
    build/framework/cxxtoolchain.hpp $
    build/framework/flags.hpp $
    build/framework/glob.hpp $
    build/framework/graph.hpp $
    build/framework/library.hpp $
    build/framework/path.hpp $
    build/framework/platform.hpp $
    build/framework/program.hpp $
    build/framework/sharedlibrary.hpp $
    build/framework/staticlibrary.hpp $
    build/framework/target.hpp $
    build/framework/tool.hpp $
    build/framework/toolchain.hpp $
    build/framework/toolchainhelpers.hpp

default _out/ngen-build-graph
)");
    if (!written) {
        std::cerr << written.error().message << "\n";
        return 1;
    }
    return std::system("ninja -f _out/ngen-build-graph.ninja") == 0 ? 0 : 1;
}
