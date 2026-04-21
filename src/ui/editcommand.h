#pragma once

#include "scenehandles.h"
#include "scenetypes.h"
#include "usdscene.h" // SceneEditRequestContext::Purpose

#include <glm/glm.hpp>

#include <optional>
#include <string>

struct SceneEditCommand {
    enum class Type {
        MuteLayer,
        SetTransform,
        SetVisibility,
        AddSubLayer,
        ClearSession,
        CreatePrim,          // typed-prim creation under parentPath
        CreateReferencePrim, // prim + reference arc; one atomic edit so undo works
        RemovePrim,          // delete prim (inverse requires subtree snapshot)
        SetDisplayColor,     // primvars:displayColor — drives gprim vertex color
    };
    Type type;
    LayerHandle layer;
    PrimHandle prim;
    Transform transform;
    bool boolValue = false;
    std::string stringValue;
    // Populated only for CreatePrim / CreateReferencePrim / RemovePrim.
    std::string parentPath;     // absolute USD path of the parent
    std::string primName;       // child name (valid USD identifier)
    std::string typeName;       // e.g. "Xform", "SphereLight"; empty for referenced prims
    std::string referenceAsset; // asset path (relative or absolute) for CreateReferencePrim
    glm::vec3 colorValue = glm::vec3(1.0f); // for SetDisplayColor
    // Authoring (default) writes to the USD layer. Preview only updates the
    // runtime transform cache — used during interactive operations like a gizmo
    // drag, where every-frame USD writes are wasteful. The final Authoring edit
    // is sent on operation end (drag release) to commit the change.
    SceneEditRequestContext::Purpose purpose = SceneEditRequestContext::Purpose::Authoring;
    // Set when the cmd is being replayed from the undo/redo stack. Tells
    // SceneUpdater not to record it again (would create a self-referential
    // history loop). Default false for normal user edits.
    bool fromHistory = false;
    // Optional pre-operation snapshot used by the undo stack to compute the
    // reverse cmd. When unset, the stack reads current scene state at apply
    // time — but for interactive operations that emit Preview edits before
    // their final Authoring commit, the cache has already been mutated by
    // those Previews, so reading at apply time captures the *post-edit* state
    // (no-op undo). Callers that know the true pre-operation state should
    // populate the matching field below.
    std::optional<Transform> inverseTransform;
    std::optional<bool> inverseBoolValue;
};
