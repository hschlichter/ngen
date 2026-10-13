#pragma once

#include <filesystem>
#include <span>
#include <string>

class AssetClient;

// Makes USD read every asset through the asset server: layers, references, payloads and textures come streamed through
// `client`, and writes (layer saves) go to the file at the asset's id, relative to the working directory. Identifiers are asset
// ids. Registers the resolver's plugin from <binDirectory>/usdplugins and makes it USD's preferred resolver; call it before
// anything uses USD. `client` must be connected and outlive every stage. See src/scene/README.md.
auto registerAssetResolver(AssetClient* client, const std::filesystem::path& binDirectory) -> bool;

// Requests every asset in `ids` from the asset server in one request, so the server packs and streams them together. A
// later open of one of them waits for it instead of requesting it again. Ids already being fetched are skipped.
auto prefetchAssets(std::span<const std::string> ids) -> void;
