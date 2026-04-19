#pragma once

#include "assetbrowser.h"
#include "editcommand.h"

class USDScene;

// Standalone window hosting the asset browser. Separate from the Layers panel so the
// same browser widget can be reused by the prim-creation reference picker (planned)
// without being tied to any one owning panel. Primary action in v1: "Add as Sublayer".
void drawAssetBrowserWindow(bool& show,
                            USDScene& usdScene,
                            AssetBrowserState& browser,
                            std::vector<SceneEditCommand>& pendingEdits);
