#pragma once

#include <filesystem>

class AssetClient;

// Makes USD read every asset through the asset server: layers, references, payloads and textures come streamed through
// `client`, and writes (layer saves) go to the file at the asset's id, relative to the working directory. Identifiers are asset
// ids. Registers the resolver's plugin from <binDirectory>/usdplugins and makes it USD's preferred resolver; call it before
// anything uses USD. `client` must be connected and outlive every stage. See src/scene/README.md.
auto registerAssetResolver(AssetClient* client, const std::filesystem::path& binDirectory) -> bool;
