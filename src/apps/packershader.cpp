// ngen-packer-shader: packs one GLSL shader into SPIR-V.
//
// Runs glslc on the source with the rule's parameters: optimize (0 = -O0, 1 = -O) and debug_info (1 = -g). glslc
// writes the SPIR-V straight to --out, and its depfile (the source and its #includes) to --depfile. The command is
// otherwise the one the build used before shaders became packed assets, so the SPIR-V is byte for byte the same.

#include "packer.h"

#include <cstdlib>
#include <print>
#include <string>
#include <vector>

namespace {

auto shellQuote(const std::string& value) -> std::string {
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    return out + "'";
}

} // namespace

auto main(int argc, char** argv) -> int {
    auto args = parsePackerArgs(argc, argv);
    if (!args) {
        std::println(stderr, "ngen-packer-shader: {}", args.error());
        return 2;
    }
    std::vector<std::string> glslc = {"glslc", args->source, "-o", args->out};
    glslc.emplace_back(args->param("optimize", "0") == "1" ? "-O" : "-O0");
    if (args->param("debug_info", "0") == "1") {
        glslc.emplace_back("-g");
    }
    glslc.emplace_back("-MD");
    glslc.emplace_back("-MF");
    glslc.push_back(args->depfile);
    std::string command;
    for (const auto& token : glslc) {
        command += (command.empty() ? "" : " ") + shellQuote(token);
    }
    if (std::system(command.c_str()) != 0) { // NOLINT(bugprone-command-processor): fixed program, quoted arguments
        std::println(stderr, "ngen-packer-shader: glslc failed on {}", args->source);
        return 1;
    }
    return 0;
}
