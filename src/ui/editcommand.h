#pragma once

#include "scenehandles.h"
#include "scenetypes.h"

#include <string>

struct SceneEditCommand {
    enum class Type {
        MuteLayer,
        SetTransform,
        SetVisibility,
        AddSubLayer,
        ClearSession
    };
    Type type;
    LayerHandle layer;
    PrimHandle prim;
    Transform transform;
    bool boolValue = false;
    std::string stringValue;
};
