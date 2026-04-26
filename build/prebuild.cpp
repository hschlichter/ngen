#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>

namespace {

bool write_if_changed(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ifstream in(path);
    std::ostringstream current;
    current << in.rdbuf();
    if (in && current.str() == text) {
        return false;
    }
    std::ofstream out(path);
    out << text;
    return true;
}

} // namespace

int main() {
    write_if_changed("_out/ngen-build-graph.ninja", R"(cxx = clang++
cxxflags = -std=c++23 -O0 -g -Wall -Wextra -Ibuild/framework
builddir = _out/.ninja

rule cxx
  command = mkdir -p _out && $cxx $cxxflags -o $out build/build.cpp build/framework/build.cpp
  description = GRAPH $out

build _out/ngen-build-graph: cxx build/build.cpp build/framework/build.hpp build/framework/build.cpp

default _out/ngen-build-graph
)");
    return std::system("ninja -f _out/ngen-build-graph.ninja") == 0 ? 0 : 1;
}
