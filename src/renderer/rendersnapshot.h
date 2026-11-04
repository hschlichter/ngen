#pragma once

#include "debugdraw.h"
#include "debugview.h"
#include "gizmo.h"
#include "imguibackend.h"
#include "lightingpass.h"

#include "shadowcascades.h"

#include <glm/glm.hpp>

#include <optional>
#include <utility>
#include <vector>

// Material sampler state, editable live from the Render Debug View tab and the
// `sampler` session verb.
struct SamplerSettings {
    float maxAnisotropy = 8.0f; // 0 = off
    float lodBias = 0.0f;
    float minLod = 0.0f; // forces coarser levels
    bool nearestMip = false;

    auto operator==(const SamplerSettings&) const -> bool = default;
};

struct RenderSnapshot {
    glm::mat4 viewMatrix;
    glm::mat4 projMatrix;
    // Where render LOD measures from: the main camera's eye, which a debug camera rendered from (viewMatrix) leaves where it is.
    glm::vec3 lodEye = glm::vec3(0.0f);
    glm::vec3 worldUp = glm::vec3(0.0f, 1.0f, 0.0f); // stage's up axis (from USDScene::worldUp())

    int windowWidth = 0;
    int windowHeight = 0;

    float mouseX = 0;
    float mouseY = 0;

    bool showGizmo = true;
    GBufferView gbufferViewMode = GBufferView::Lit;
    DebugView debugView = DebugView::None; // replaces the lit image when on (DebugViewPass)
    // Per streaming root slot, its state (StreamingRootState, with streamingStateFinding), for the streaming view; empty while it is off.
    std::vector<uint32_t> streamingStates;
    bool showBufferOverlay = false;
    bool showShadowOverlay = false;
    bool antiAliasing = true;
    bool depthPrepass = false; // off by default: costs more than it saves without heavy overdraw
    SamplerSettings sampler;

    // GPU culling inputs: the camera frustum to cull against (live,
    // or frozen from the editor) and whether culling is on.
    bool cullEnabled = true;
    glm::mat4 cullViewProj = glm::mat4(1.0f);
    // Read every instance's visibility bits per view back with the culling readback (CullResult::viewBits, the culled overlay).
    bool readVisibility = false;
    // Read every render-LOD group's level drawn and wanted back with the culling readback (for the `renderlod` record).
    bool readLodStates = false;
    // Render LOD's tuning: the margin at a boundary without a blend band, as a fraction of a screen size (a distance's is the same margin
    // in the size: 1 / sqrt(1 - h) farther); and the pop flash, tinting a group's meshes after its level changed without a fade.
    float lodHysteresis = 0.2f;
    bool lodPopFlash = false;
    bool lodColorByDepth = false; // the LOD Level view coloured by how many LOD roots deep a draw is, not by its drawn level
    uint32_t lodFlashFrames = 30; // half a second at the view's frame rate, at most 127

    // Shadow cascades fitted on the main thread, culled on the GPU.
    // cascadeCount 0 means the renderer fits a single cascade itself.
    ShadowCascadeSettings shadowSettings;
    AmbientSettings ambient;
    uint32_t cascadeCount = 0;
    std::array<ShadowCascade, maxShadowCascades> cascades;
    // The camera the cascades were fitted to, when it isn't the one rendered from (a debug camera watching the main camera): the lighting
    // pass picks a pixel's cascade by its depth from this camera instead.
    std::optional<std::pair<glm::vec3, glm::vec3>> cascadeCamera; // eye, forward

    std::vector<GizmoVertex> translateGizmoVerts;
    std::vector<GizmoVertex> rotateGizmoVerts;
    std::vector<GizmoVertex> scaleGizmoVerts;

    DebugDrawData debugData;
    ImGuiFrameSnapshot imguiSnapshot;
};
