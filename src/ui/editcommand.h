#pragma once

#include "scenehandles.h"
#include "scenetypes.h"
#include "usdscene.h" // SceneEditRequestContext::Purpose

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
    // Authoring (default) writes to the USD layer. Preview only updates the
    // runtime transform cache — used during interactive operations like a gizmo
    // drag, where every-frame USD writes are wasteful. The final Authoring edit
    // is sent on operation end (drag release) to commit the change.
    SceneEditRequestContext::Purpose purpose = SceneEditRequestContext::Purpose::Authoring;
};
