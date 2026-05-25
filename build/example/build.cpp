// Minimal `build.cpp` for the build system's `build/example/` demo.
//
// This file is the project graph for a hello-world program. Every line below describes "what to build" and
// "how to build it"; the build system reads this graph and produces the IR + runs the compiles. See the
// sibling `README.md` for how to actually invoke it.

#include "../framework/cxx/configuration.hpp"
#include "../framework/cxx/platform.hpp"
#include "../framework/cxx/target.hpp"
#include "../framework/project.hpp"
#include "../ir/main.hpp"

using namespace build;

auto main(int argc, char** argv) -> int {
    // The toolchain: which compiler, archiver, default C++ standard.
    auto clang = cxx::toolchain()
                     .compiler("clang++")
                     .archiver("ar")
                     .default_std("c++23");

    // One platform. Use any name — it's the string the user passes to `-p`.
    auto host = cxx::platform("host")
                    .os("linux")
                    .toolchain(clang)
                    .compile_flag("-Wall");

    // Two configurations.
    auto debug = cxx::configuration("debug")
                     .compile_flag("-O0")
                     .compile_flag("-g")
                     .define("DEBUG=1");

    auto release = cxx::configuration("release")
                       .compile_flag("-O2")
                       .define("NDEBUG");

    // Register them with the project.
    Project p;
    p.platform(host);
    p.config(debug);
    p.config(release);

    // One program with one source. `sources` materialises a `cxx::ObjectFile` child per `.cpp`, dep-edged
    // into the program's link.
    auto hello = cxx::program("hello").sources({"src/main.cpp"});

    p.target(hello);
    p.default_target(hello);

    // Hand off argv parsing + dispatch to the framework's graph-stage main.
    return ir::main(argc, argv, p);
}
