#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <cstdlib>
#include <vector>

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

std::string shell_quote(const std::string& value) {
    if (value.empty()) {
        return "''";
    }
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

struct Args {
    std::string target = "ngen-view";
    std::string platform;
    std::string config;
    std::string backend = "ninja";
    int verbosity = 0;
};

Args parse(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + arg);
            }
            return argv[++i];
        };
        if (arg == "--platform") {
            args.platform = value();
        } else if (arg == "--config" || arg == "-c") {
            args.config = value();
        } else if (arg == "--backend") {
            args.backend = value();
        } else if (arg == "-v" || arg == "--verbose") {
            args.verbosity = std::max(args.verbosity, 1);
        } else if (arg == "-vv") {
            args.verbosity = std::max(args.verbosity, 2);
        } else {
            args.target = arg;
        }
    }
    return args;
}

std::string forward_args(int argc, char** argv) {
    std::string out;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose" || arg == "-vv") {
            continue;
        }
        out += " ";
        out += shell_quote(argv[i]);
    }
    return out;
}

std::string ninja_target(const Args& args) {
    if (args.target == "clean" || args.target == "format" || args.target == "tidy") {
        return args.target;
    }
    if (args.platform.empty() && args.config.empty()) {
        return args.target;
    }
    auto platform = args.platform.empty() ? "linux-vulkan" : args.platform;
    auto config = args.config.empty() ? "debug" : args.config;
    return args.target + ":" + platform + ":" + config;
}

} // namespace

int main(int argc, char** argv) {
    try {
        auto args = parse(argc, argv);
        if (args.backend != "ninja") {
            std::cerr << "unsupported backend: " << args.backend << "\n";
            return 1;
        }

        write_if_changed("_out/ngen-build-pre.ninja", R"(cxx = clang++
cxxflags = -std=c++23 -O0 -g -Wall -Wextra
builddir = _out/.ninja

rule cxx
  command = mkdir -p _out && $cxx $cxxflags -o $out $in
  description = PREBUILD $out

build _out/ngen-build-pre: cxx build/prebuild.cpp

default _out/ngen-build-pre
)");

        auto prebuild_cmd = std::string(args.verbosity == 1 ? "TERM=dumb ninja -f _out/ngen-build-pre.ninja" : "ninja -f _out/ngen-build-pre.ninja");
        if (args.verbosity >= 2) {
            prebuild_cmd += " -v";
        }
        if (std::system(prebuild_cmd.c_str()) != 0) {
            return 1;
        }
        if (std::system("./_out/ngen-build-pre") != 0) {
            return 1;
        }

        auto graph_cmd = "./_out/ngen-build-graph" + forward_args(argc, argv);
        if (std::system(graph_cmd.c_str()) != 0) {
            return 1;
        }

        auto build_cmd = std::string(args.verbosity == 1 ? "TERM=dumb ninja -f _out/build.ninja" : "ninja -f _out/build.ninja");
        if (args.verbosity >= 2) {
            build_cmd += " -v";
        }
        build_cmd += " " + shell_quote(ninja_target(args));
        return std::system(build_cmd.c_str()) == 0 ? 0 : 1;
    } catch (const std::exception& err) {
        std::cerr << err.what() << "\n";
        return 1;
    }
}
