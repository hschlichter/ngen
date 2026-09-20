#pragma once

#include "rhitypes.h"

#include <shaderc/shaderc.h>

#include <cstddef>
#include <cstring>
#include <print>
#include <string_view>
#include <vector>

// GLSL source to SPIR-V via shaderc. Prints the compiler log to stderr and returns
// an empty vector on error. Examples embed their shaders as strings so the whole
// program reads as one file; the engine compiles offline instead. Header-only so
// each example is a single translation unit.
inline auto compileGlsl(RhiShaderStage stage, std::string_view source, const char* name) -> std::vector<std::byte> {
    auto kind = shaderc_glsl_vertex_shader;
    if (stage == RhiShaderStage::Fragment) {
        kind = shaderc_glsl_fragment_shader;
    } else if (stage == RhiShaderStage::Compute) {
        kind = shaderc_glsl_compute_shader;
    }

    auto* compiler = shaderc_compiler_initialize();
    auto* options = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    // Same policy as the engine's glslc flags in build.cpp: optimise in release, debug info otherwise.
#ifdef NDEBUG
    shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_performance);
#else
    shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_zero);
    shaderc_compile_options_set_generate_debug_info(options);
#endif

    auto* result = shaderc_compile_into_spv(compiler, source.data(), source.size(), kind, name, "main", options);

    std::vector<std::byte> spirv;
    auto status = shaderc_result_get_compilation_status(result);
    if (status != shaderc_compilation_status_success) {
        std::println(stderr, "Shader compile failed ({}):\n{}", name, shaderc_result_get_error_message(result));
    } else {
        auto length = shaderc_result_get_length(result);
        spirv.resize(length);
        memcpy(spirv.data(), shaderc_result_get_bytes(result), length);
    }

    shaderc_result_release(result);
    shaderc_compile_options_release(options);
    shaderc_compiler_release(compiler);
    return spirv;
}
