// The project's pack rules: which file extensions can be packed, and the packer program that packs each.
// Compiled into ngen-asset-server (src/asset/README.md). Any file with one of these extensions can be
// requested; no asset is listed here.

#include "packrule.h"

auto packRules(const std::string& config) -> std::vector<PackRule> {
    std::vector<PackRule> rules;
    // Shader parameters follow the configuration the way compiler flags do: debug keeps source-level debug
    // info and no optimisation, release optimises and keeps debug info, gamerelease optimises only.
    rules.push_back(PackRule{
        .name = "shader",
        .extensions = {".vert", ".frag", ".comp", ".geom"},
        .packer = "ngen-packer-shader",
        .params = {
            {"optimize", config == "debug" ? "0" : "1"},
            {"debug_info", config == "gamerelease" ? "0" : "1"},
        },
        .version = 1,
    });
    // Images become engine-ready textures: RGBA8 sRGB with the full mip chain, decoded and mip-mapped once, here.
    rules.push_back(PackRule{
        .name = "texture",
        .extensions = {".png", ".jpg", ".jpeg"},
        .packer = "ngen-packer-texture",
        .params = {},
        .version = 1,
    });
    // Assets the engine still reads in their source format, until a packer for their engine-ready format exists: the packed
    // asset is the source's bytes.
    rules.push_back(PackRule{
        .name = "copy",
        .extensions = {".usda", ".usdc", ".usd", ".hdr"},
        .packer = "ngen-packer-copy",
        .params = {},
        .version = 1,
    });
    return rules;
}
