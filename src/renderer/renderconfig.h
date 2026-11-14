#pragma once

#include "config.h"
#include "lightingpass.h"
#include "rendersnapshot.h"
#include "shadowcascades.h"

// The renderer's configuration values (src/config/README.md), each defined in the file of the part it configures. The renderer itself
// never reads them: the main thread does, filling the RenderSnapshot and the render thread's settings, as the configuration's values are
// the main thread's.
namespace renderconfig {

// lightingpass.cpp
extern config::Enum bufferView; // GBufferView, by gbufferViewNames
extern config::Int hzbLevel;    // render.view.hzblevel: the pyramid level the hzb buffer view shows
extern config::Bool bufferOverlay;
extern config::Bool shadowOverlay;
extern config::Vec3 ambientSky;
extern config::Vec3 ambientGround;
extern config::Bool skyOn; // render.sky.on: the dome light as sky and ambient
// debugviewpass.cpp
extern config::Enum debugView; // DebugView, by debugViewNames
// renderer.cpp
extern config::Bool antiAliasing;
extern config::Bool depthPrepass;
extern config::Float samplerAnisotropy;
extern config::Float samplerLodBias;
extern config::Float samplerMinLod;
extern config::Bool samplerNearestMip;
// shadowcascades.cpp
extern config::Int shadowCascades;
extern config::Int shadowTile;
extern config::Float shadowSplitLambda;
extern config::Bool shadowPcf;
extern config::Float shadowDistance;
// culling.cpp
extern config::Bool cullingEnabled;
extern config::Bool cullingFrozen;
extern config::Bool cullingOcclusion; // render.culling.occlusion: two-phase occlusion culling against the depth pyramid
// gpuscene.cpp
extern config::Int uploadBudgetMiB;
extern config::Int geometryLimitMiB;
// buffergrowth.cpp
extern config::Float growthAhead; // render.growth.ahead

// The groups the snapshot carries as structs, read from and written to their values (a window editing a group sets each value).
auto shadowSettings() -> ShadowCascadeSettings;
auto setShadowSettings(const ShadowCascadeSettings& settings) -> void;
auto samplerSettings() -> SamplerSettings;
auto setSamplerSettings(const SamplerSettings& settings) -> void;
auto ambient() -> AmbientSettings;
auto setAmbient(const AmbientSettings& settings) -> void;

} // namespace renderconfig
