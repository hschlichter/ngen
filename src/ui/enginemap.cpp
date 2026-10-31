#include "enginemap.h"

#include <imgui.h>

#include <array>
#include <format>
#include <utility>

namespace {

constexpr std::array<const char*, 5> lanes = {
    "ngen-asset-server (process)",
    "Asset client (I/O thread)",
    "Scene thread",
    "Main thread",
    "Render thread",
};

const std::vector<EngineMapNode> nodes = {
    // The asset server: its own process, in the directory every asset id is relative to.
    {
        .id = "assets.watcher",
        .label = "File watcher",
        .lane = 0,
        .row = 0,
        .description = "Watches the sources of every packed asset (inotify, polling as the fallback). A changed source is repacked and every "
                       "client that asked for it is told (asset.changed).",
        .records = {"status"},
        .windows = {},
        .readme = "src/asset/README.md",
    },
    {
        .id = "assets.packing",
        .label = "Pack jobs",
        .lane = 0,
        .row = 1,
        .description = "Turns a source into engine-ready data: pack.cpp's rules pick a packer program per file type, one job per file, on a "
                       "pool of workers. The USD packer writes layer, mesh and material packs; textures and shaders have their own packers.",
        .records = {"status", "rules", "requests"},
        .windows = {},
        .readme = "src/asset/README.md",
    },
    {
        .id = "assets.cache",
        .label = "Pack cache",
        .lane = 0,
        .row = 2,
        .description = "Every packed asset by id and version (.ngen-assets/ in the server's directory). A request for an asset whose inputs "
                       "haven't changed is answered from here without packing.",
        .records = {"cache"},
        .windows = {},
        .readme = "src/asset/README.md",
    },
    {
        .id = "assets.delivery",
        .label = "Delivery",
        .lane = 0,
        .row = 3,
        .description = "Streams packed bytes to each client over TCP on loopback, in chunks within a window per client.",
        .records = {"clients", "requests"},
        .windows = {},
        .readme = "src/asset/README.md",
    },
    // The view's asset client.
    {
        .id = "view.assetclient",
        .label = "Asset client",
        .lane = 1,
        .row = 3,
        .description = "ngen-view's connection to the asset server: sends asset.request, takes asset.data and asset.changed, and wakes the "
                       "scene thread (and the render thread, for shaders) when something is answered.",
        .records = {"assets"},
        .windows = {"assets"},
        .readme = "src/asset/README.md",
    },
    // The scene thread.
    {
        .id = "scene.lod",
        .label = "LOD selection",
        .lane = 2,
        .row = 3,
        .description = "The LOD heuristic for streaming roots (LOD roots whose levels are payloads; render-LOD roots are picked on the GPU, "
                       "see LOD select): each one reached gets the level its heuristic picks from the camera (UsdLod's distance or screen size, "
                       "by the preference: distance first by default; inside an authored blend band the finer level, without one the "
                       "hysteresis margin before going coarser), unless set by hand or the heuristic is off. The view's LOD tuning (bias, "
                       "hysteresis, blending, thresholds per authored root; never saved) applies here and to the render-LOD groups. "
                       "A root draws its picked level once that level's placement waits for nothing (composed, its meshes, materials and "
                       "textures held), else the nearest ready coarser one, never a finer one. What is resident is Residency's.",
        .records = {"streaming", "lod", "lodtool"},
        .windows = {"lod"},
        .readme = "src/view/README.md",
    },
    {
        .id = "scene.residency",
        .label = "Residency",
        .lane = 2,
        .row = 4,
        .description = "Which levels of each streaming root are loaded, from where the camera is and what is authored, never from time: the "
                       "root's streaming heuristic (load and unload radii) gives its state, the finest level that must be resident, with the "
                       "previous state as hysteresis; that level and every coarser one are resident (without one, the level picked and every "
                       "coarser one). Changed levels are linked again without composing the stage; a level released goes with its placement, "
                       "contents and assets. It checks the authoring contract per root and measures late draws, holes, lead and reloads; "
                       "nothing acts on the measures.",
        .records = {"streaming", "streamingtool"},
        .windows = {"streaming"},
        .readme = "src/view/README.md",
    },
    {
        .id = "scene.compose",
        .label = "Compose",
        .lane = 2,
        .row = 5,
        .description = "WorldComposer composes the World from the layer packs held, as USD composes a stage (sublayers, references, payloads, "
                       "variant sets, instancing, point instancers, LOD levels, material inputs), and asks for the packs it names and lacks: one "
                       "level of arcs deeper each round. A content is linked once complete; one no level reaches any more is released at once.",
        .records = {"scenethread", "scene", "lod", "variants"},
        .windows = {"sceneflow", "lod"},
        .readme = "src/world/README.md",
    },
    {
        .id = "scene.placement",
        .label = "Placement",
        .lane = 2,
        .row = 6,
        .description = "A tree of placements of the World's contents (the stage, a prototype at each instance, each loaded LOD level at its root, "
                       "a point instancer's points), each owning the draws of its own prims; a pass builds again only what changed and looks only "
                       "where something waits or changed, and its slot changes are the change lists. Meshes, materials and textures are kept per "
                       "asset in libraries while a placement uses them, drawn or not (a level loading, a resident level not drawn), so "
                       "switching levels uploads nothing. Each draw carries its streaming root's slot, for the streaming view.",
        .records = {"scenethread", "lod", "payloads", "instancers", "variants"},
        .windows = {"sceneflow", "lod"},
        .readme = "src/view/README.md",
    },
    {
        .id = "scene.changes",
        .label = "Change lists",
        .lane = 2,
        .row = 7,
        .description = "What a placement pass changed (draws, library entries, lights), published with a snapshot of the scene for the "
                       "main thread's queries, which shares the tree and the slots with the scene thread.",
        .records = {"scenethread"},
        .windows = {"sceneflow"},
        .readme = "src/view/README.md",
    },
    // The main thread.
    {
        .id = "main.rpc",
        .label = "RPC and scripts",
        .lane = 3,
        .row = 0,
        .description = "The RPC endpoint (view.* methods, records, the trace stream) and --script verbs, which call the same methods. "
                       "Methods run at a fixed point in the frame; ngen-introspect and agents connect here.",
        .records = {},
        .windows = {},
        .readme = "src/rpc/README.md",
    },
    {
        .id = "main.input",
        .label = "Input and camera",
        .lane = 3,
        .row = 1,
        .description = "SDL events: fly camera (WASD, mouse look, the wheel for speed), picking, gizmos, shortcuts. Two cameras: the "
                       "main camera (the player), which scene cameras, bookmarks, scripts and fly place, and from which residency, LOD and "
                       "culling are decided; and the debug camera, rendered from while on, the main camera drawn in it with its trail "
                       "(C hands the controls between them). Modes for the camera with the controls: Left Alt held orbits the surface "
                       "under the cursor (the pick's depth); G follows the ground, its eye kept a height above the highest drawn surface "
                       "under it (a ray down through the draws, skipped by chunk bounds, every triangle of those left).",
        .records = {"status"},
        .windows = {"camera"},
        .readme = "src/view/README.md",
    },
    {
        .id = "main.ui",
        .label = "UI windows",
        .lane = 3,
        .row = 2,
        .description = "Dear ImGui windows over the view: they read the records' data and change settings; their draw data goes to the "
                       "render thread with the frame.",
        .records = {"status"},
        .windows = {},
        .readme = "src/view/README.md",
    },
    {
        .id = "main.scene",
        .label = "Scene snapshot and residency",
        .lane = 3,
        .row = 6,
        .description = "Once a frame the main thread takes the scene thread's latest snapshot and change lists and hands the changes to the "
                       "render thread. Resident: the scene thread is idle and the render thread has drawn every change; script frames "
                       "count only then.",
        .records = {"scenethread", "status"},
        .windows = {"sceneflow"},
        .readme = "src/view/README.md",
    },
    {
        .id = "main.frame",
        .label = "Frame snapshot",
        .lane = 3,
        .row = 7,
        .description = "The frame's RenderSnapshot: camera, shadow cascades fitted here, view mode, sampler, ambient and other settings, "
                       "gizmos and UI draw data. One in flight: the main thread waits if the render thread is behind.",
        .records = {"status"},
        .windows = {"renderdebug"},
        .readme = "src/renderer/README.md",
    },
    // The render thread.
    {
        .id = "render.pipelines",
        .label = "Pipelines",
        .lane = 4,
        .row = 3,
        .description = "PipelineRegistry: each pipeline is made once its shaders (assets, SPIR-V from the asset server) arrive; a pass "
                       "whose pipeline isn't ready is skipped. A changed shader rebuilds its pipelines.",
        .records = {"pipelines"},
        .windows = {"shaders"},
        .readme = "src/renderer/README.md",
    },
    {
        .id = "render.gpuscene",
        .label = "GPU scene",
        .lane = 4,
        .row = 6,
        .description = "Applies change lists to the GPU tables: instance slots, the mesh and material tables, texture slots. Removals "
                       "first, so additions reuse their room. Everything it writes goes through the upload queue.",
        .records = {"gpuscene", "render"},
        .windows = {"sceneflow", "gpuscene"},
        .readme = "src/renderer/README.md",
    },
    {
        .id = "render.uploads",
        .label = "Upload queue",
        .lane = 4,
        .row = 7,
        .description = "Copies to the GPU, in order: mesh data, table entries, texture levels. The SceneUpload pass records at most the "
                       "budget per frame (64 MiB by default), before culling, so nothing waits.",
        .records = {"gpuscene"},
        .windows = {"sceneflow"},
        .readme = "src/renderer/README.md",
    },
    {
        .id = "render.pool",
        .label = "Geometry pool",
        .lane = 4,
        .row = 8,
        .description = "Every mesh's vertices and indices, a range each, in three buffers that grow by doubling up to a limit. A mesh that "
                       "doesn't fit waits, undrawn, until removals make room.",
        .records = {"gpuscene", "memory"},
        .windows = {"sceneflow", "memory"},
        .readme = "src/renderer/README.md",
    },
    {
        .id = "render.lodselect",
        .label = "LOD select",
        .lane = 4,
        .row = 9,
        .description = "Render LOD: per group (a render-LOD root where it is placed) the level its heuristic (UsdLod's distance or screen "
                       "size, as the view's preference chose) wants from the live camera, or the level set by hand; drawn when its meshes are "
                       "in the pool, else the level drawn last frame, else the nearest resident one. Inside an authored blend band the next "
                       "level fades in: culling draws both levels in the cutout buckets, whose pipelines dither them by the band's share. A "
                       "drawn level changed without a fade is a pop: the group flashes (the LOD window's pop flash) and is counted. The "
                       "hysteresis is the view's tuning, a push constant. A compute pass before culling.",
        .records = {"renderlod", "lodtool", "culling"},
        .windows = {"lod", "culling"},
        .readme = "src/renderer/README.md",
    },
    {
        .id = "render.framegraph",
        .label = "Frame graph",
        .lane = 4,
        .row = 10,
        .description = "The frame's passes, ordered by what they read and write: SceneUpload, GPU culling per view, shadow cascades, the "
                       "G-buffer, lighting, anti-aliasing, debug lines and UI. Barriers come from the graph.",
        .records = {"render", "culling", "counters", "profile"},
        .windows = {"framegraph", "framedebugger", "culling", "counters"},
        .readme = "src/renderer/README.md",
    },
    {
        .id = "render.present",
        .label = "Present",
        .lane = 4,
        .row = 11,
        .description = "The frame goes to the swapchain (a window, or offscreen for headless runs and screenshots).",
        .records = {"profile"},
        .windows = {"performance"},
        .readme = "src/rhi/README.md",
    },
};

const std::vector<EngineMapEdge> edges = {
    {.from = "assets.watcher", .to = "assets.packing", .what = "changed sources are repacked"},
    {.from = "assets.packing", .to = "assets.cache", .what = "packed files"},
    {.from = "assets.cache", .to = "assets.delivery", .what = "packed bytes"},
    {.from = "assets.delivery", .to = "view.assetclient", .what = "asset.data, asset.ready (TCP)"},
    {.from = "assets.watcher", .to = "view.assetclient", .what = "asset.changed"},
    {.from = "view.assetclient", .to = "scene.compose", .what = "layer packs; answers wake the scene thread"},
    {.from = "view.assetclient", .to = "scene.placement", .what = "mesh, material and texture packs"},
    {.from = "view.assetclient", .to = "render.pipelines", .what = "shaders (SPIR-V); changed ones reload"},
    {.from = "scene.compose", .to = "view.assetclient", .what = "asset.request for the packs it names"},
    {.from = "scene.compose", .to = "scene.placement", .what = "the World"},
    {.from = "scene.placement", .to = "scene.changes", .what = "slots, LOD groups, library entries, lights"},
    {.from = "main.rpc", .to = "scene.compose", .what = "requests: open, purposes, repack"},
    {.from = "main.rpc", .to = "scene.lod", .what = "levels set by hand, heuristic on or off, tuning and threshold overrides"},
    {.from = "main.input", .to = "scene.lod", .what = "main camera (once placed)"},
    {.from = "scene.lod", .to = "scene.compose", .what = "levels picked to draw"},
    {.from = "scene.lod", .to = "scene.residency", .what = "the level picked, for roots without a streaming heuristic"},
    {.from = "main.input", .to = "scene.residency", .what = "main camera (once placed)"},
    {.from = "main.rpc", .to = "scene.residency", .what = "radii tuning, counters reset, fly"},
    {.from = "scene.residency", .to = "scene.compose", .what = "resident levels: linked again, their packs requested"},
    {.from = "scene.placement", .to = "scene.residency", .what = "the streaming roots drawn, levels ready"},
    {.from = "scene.residency", .to = "main.ui", .what = "the streaming record: rows, map, states for the streaming view"},
    {.from = "scene.placement", .to = "scene.lod", .what = "the LOD roots drawn"},
    {.from = "scene.changes", .to = "main.scene", .what = "snapshot and change lists"},
    {.from = "main.scene", .to = "render.gpuscene", .what = "change lists (submitSceneChanges)"},
    {.from = "render.gpuscene", .to = "main.scene", .what = "drawn and idle: residency"},
    {.from = "main.input", .to = "main.frame", .what = "the camera rendered from, the main camera's eye, the culling camera"},
    {.from = "main.scene", .to = "main.input", .what = "the draws and their meshes, for the ground under the camera"},
    {.from = "main.ui", .to = "main.frame", .what = "settings, UI draw data"},
    {.from = "main.frame", .to = "render.framegraph", .what = "RenderSnapshot"},
    {.from = "render.gpuscene", .to = "render.uploads", .what = "queued copies"},
    {.from = "render.uploads", .to = "render.pool", .what = "vertices and indices"},
    {.from = "render.pool", .to = "render.framegraph", .what = "geometry and tables, read by culling and the passes"},
    {.from = "render.pipelines", .to = "render.framegraph", .what = "pipelines"},
    {.from = "render.gpuscene", .to = "render.lodselect", .what = "the LOD group table"},
    {.from = "main.frame", .to = "render.lodselect", .what = "the main camera's eye (lodEye)"},
    {.from = "render.lodselect", .to = "render.framegraph", .what = "the level each group draws, read by culling"},
    {.from = "render.framegraph", .to = "render.present", .what = "the frame"},
};

std::string focusRequest;

} // namespace

auto engineMapLanes() -> std::span<const char* const> {
    return lanes;
}

auto engineMapNodes() -> std::span<const EngineMapNode> {
    return nodes;
}

auto engineMapEdges() -> std::span<const EngineMapEdge> {
    return edges;
}

auto engineMapNode(const std::string& id) -> const EngineMapNode* {
    for (const auto& node : nodes) {
        if (id == node.id) {
            return &node;
        }
    }
    return nullptr;
}

auto requestEngineMapFocus(const char* nodeId) -> void {
    focusRequest = nodeId;
}

auto takeEngineMapFocus() -> std::string {
    return std::exchange(focusRequest, {});
}

auto drawEngineMapLink(const char* nodeId) -> void {
    const auto* node = engineMapNode(nodeId);
    if (node == nullptr) {
        return;
    }
    auto label = std::format("Map: {} > {}###maplink", lanes[(size_t) node->lane], node->label);
    if (ImGui::SmallButton(label.c_str())) {
        requestEngineMapFocus(nodeId);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Where this sits in the engine: opens the Engine Map at it");
    }
}
