#pragma once

#include <cstdint>
#include <string>
#include <vector>

class ViewScene;
struct Camera;

// Where "go to asset" is: the asset, the order it steps through its draws (nearest to the camera first, as they were at its first go-to),
// and the step. A rebuilt scene (another revision) starts the order again.
struct AssetGoToState {
    std::string assetId;
    std::vector<uint32_t> order;
    size_t index = 0;
    uint64_t revision = 0;
};

// Where a go-to went: the prim selected, and where in the asset's draws it is; `error` is set when nothing draws the asset.
struct AssetGoToResult {
    std::string prim;
    size_t index = 0;
    size_t count = 0;
    std::string error;
};

// Moves the camera to an asset (view.gotoAsset, the Assets window): the next of its draws (the first go-to picks the nearest), framed and
// its prim selected into `selectedPath`, or with `all` every draw framed together.
auto goToAsset(AssetGoToState& state, Camera& camera, const ViewScene& scene, std::string& selectedPath, const std::string& assetId, bool all) -> AssetGoToResult;
