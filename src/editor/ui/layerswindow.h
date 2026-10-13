#pragma once

#include "editcommand.h"

class USDScene;

void drawLayersWindow(bool& show, bool editingBlocked, USDScene& usdScene, std::vector<SceneEditCommand>& pendingEdits);
