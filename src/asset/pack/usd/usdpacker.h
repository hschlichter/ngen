#pragma once

#include "scenepacktypes.h"

#include <string>
#include <utility>
#include <vector>

// Packs one USD file: the opinions it authors, as a layer pack, and every mesh and material it defines, each its own asset. Nothing from
// another file is read: the layer's opinions are read as the file authors them (Sdf), and its meshes and materials through a stage of the
// file alone, every arc to another file removed, so composition inside the file (internal references, node graphs) still applies.
// See src/asset/README.md.
//
// Built as C++20, as OpenUSD's headers require; its interface is plain data.

struct UsdPackResult {
    std::string error; // set when the file can't be packed at all
    LayerPack layer;
    std::vector<std::pair<std::string, MeshPack>> meshes;        // by prim path in the file
    std::vector<std::pair<std::string, MaterialPack>> materials; // by prim path in the file
};

// `assetId` is the file's asset id (asset paths it authors are resolved against its folder), `sourcePath` where to read it.
auto packUsdFile(const std::string& assetId, const std::string& sourcePath) -> UsdPackResult;
