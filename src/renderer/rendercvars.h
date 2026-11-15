#pragma once

#include "cvar.h"
#include "lightingpass.h"
#include "rendersnapshot.h"
#include "shadowcascades.h"

// The renderer's cvars (src/cvar/README.md), each defined in the file of the part it configures. The renderer itself
// never reads them: the main thread does, filling the RenderSnapshot and the render thread's settings, as the cvars are
// the main thread's.
namespace rendercvars {

// lightingpass.cpp
extern cvar::Enum viewMode; // ViewMode, by viewModeNames
extern cvar::Int hzbLevel;  // render.view.hzblevel: the pyramid level the hzb view mode shows
extern cvar::Bool bufferOverlay;
extern cvar::Bool shadowOverlay;
extern cvar::Vec3 ambientSky;
extern cvar::Vec3 ambientGround;
extern cvar::Bool skyOn; // render.sky.on: the dome light as sky and ambient
// debugviewpass.cpp
extern cvar::Enum debugView; // DebugView, by debugViewNames
// renderer.cpp
extern cvar::Bool antiAliasing;
extern cvar::Bool depthPrepass;
extern cvar::Float samplerAnisotropy;
extern cvar::Float samplerLodBias;
extern cvar::Float samplerMinLod;
extern cvar::Bool samplerNearestMip;
// shadowcascades.cpp
extern cvar::Int shadowCascades;
extern cvar::Int shadowTile;
extern cvar::Float shadowSplitLambda;
extern cvar::Bool shadowPcf;
extern cvar::Float shadowDistance;
// culling.cpp
extern cvar::Bool cullingEnabled;
extern cvar::Bool cullingFrozen;
extern cvar::Bool cullingOcclusion; // render.culling.occlusion: two-phase occlusion culling against the depth pyramid
// gpuscene.cpp
extern cvar::Int uploadBudgetMiB;
extern cvar::Int geometryLimitMiB;
// buffergrowth.cpp
extern cvar::Float growthAhead; // render.growth.ahead

// The groups the snapshot carries as structs, read from and written to their values (a window editing a group sets each value).
auto shadowSettings() -> ShadowCascadeSettings;
auto setShadowSettings(const ShadowCascadeSettings& settings) -> void;
auto samplerSettings() -> SamplerSettings;
auto setSamplerSettings(const SamplerSettings& settings) -> void;
auto ambient() -> AmbientSettings;
auto setAmbient(const AmbientSettings& settings) -> void;

} // namespace rendercvars
