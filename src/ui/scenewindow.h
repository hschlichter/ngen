#pragma once

#include "usdscene.h"

struct RenderWorld;

void drawSceneWindow(bool& show, bool editingBlocked, USDScene& usdScene, const RenderWorld& renderWorld, PrimHandle& selectedPrim);
