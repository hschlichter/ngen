#pragma once

#include "debugdraw.h"
#include "gizmo.h"
#include "imguibackend.h"
#include "lightingpass.h"

#include "shadowcascades.h"

#include <glm/glm.hpp>

#include <vector>

// Material sampler state, editable live from the Render Debug View tab and the
// `sampler` session verb (docs/plan_mip_debug.md).
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
    glm::vec3 worldUp = glm::vec3(0.0f, 1.0f, 0.0f); // stage's up axis (from USDScene::worldUp())

    int windowWidth = 0;
    int windowHeight = 0;

    float mouseX = 0;
    float mouseY = 0;

    bool showGizmo = true;
    GBufferView gbufferViewMode = GBufferView::Lit;
    bool showBufferOverlay = false;
    bool showShadowOverlay = false;
    bool antiAliasing = true;
    bool depthPrepass = false; // off by default: costs more than it saves without heavy overdraw
    SamplerSettings sampler;

    // GPU culling inputs (docs/plan_gpu_culling.md): the camera frustum to cull against (live,
    // or frozen from the editor) and whether culling is on.
    bool cullEnabled = true;
    glm::mat4 cullViewProj = glm::mat4(1.0f);

    // Shadow cascades fitted on the main thread (docs/plan_shadow_cascades.md), culled on the GPU.
    // cascadeCount 0 means the renderer fits a single cascade itself.
    ShadowCascadeSettings shadowSettings;
    uint32_t cascadeCount = 0;
    std::array<ShadowCascade, maxShadowCascades> cascades;

    std::vector<GizmoVertex> translateGizmoVerts;
    std::vector<GizmoVertex> rotateGizmoVerts;
    std::vector<GizmoVertex> scaleGizmoVerts;

    DebugDrawData debugData;
    ImGuiFrameSnapshot imguiSnapshot;
};
