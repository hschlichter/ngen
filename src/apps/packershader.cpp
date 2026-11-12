// ngen-packer-shader: packs one GLSL shader into SPIR-V.
//
// Runs glslc on the source with the rule's parameters: optimize (0 = -O0, 1 = -O) and debug_info (1 = -g). glslc
// writes the SPIR-V straight to --out, and its depfile (the source and its #includes) to --depfile. The command is
// otherwise the one the build used before shaders became packed assets, so the SPIR-V is byte for byte the same.
// Every shader targets Vulkan 1.3: GL_EXT_mesh_shader needs SPIR-V 1.4, subgroup operations 1.3.

#include "os.h"
#include "packer.h"
#include "profile.h"

#include <cstdio>
#include <print>
#include <string>
#include <vector>

auto main(int argc, char** argv) -> int {
    // A packer runs for milliseconds: Tracy only when NGEN_TRACY=1 asks (its start-up alone takes 200 ms).
    profile::startTracyIfAsked();
    profile::registerThread("Main");
    PROFILE_ZONE("PackerMain", Assets);
    auto args = parsePackerArgs(argc, argv);
    if (!args) {
        std::println(stderr, "ngen-packer-shader: {}", args.error());
        return 2;
    }
    // Every shader targets Vulkan 1.3, ngen's baseline: mesh shading needs SPIR-V 1.4, subgroup operations 1.3.
    std::vector<std::string> glslc = {"glslc", args->source, "-o", args->out, "--target-env=vulkan1.3"};
    glslc.emplace_back(args->param("optimize", "0") == "1" ? "-O" : "-O0");
    if (args->param("debug_info", "0") == "1") {
        glslc.emplace_back("-g");
    }
    glslc.emplace_back("-MD");
    glslc.emplace_back("-MF");
    glslc.push_back(args->depfile);
    // glslc from PATH, no shell; what it prints is this packer's output, which the asset server reports.
    auto result = os::runProcess(glslc);
    std::fputs(result.output.c_str(), stderr);
    if (!result.output.empty() && result.output.back() != '\n') {
        std::fputs("\n", stderr);
    }
    if (result.exitCode != 0) {
        std::println(stderr, "ngen-packer-shader: glslc failed on {}", args->source);
        return 1;
    }
    return 0;
}
