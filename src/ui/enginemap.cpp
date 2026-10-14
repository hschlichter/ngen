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
        .id = "scene.compose",
        .label = "Compose",
        .lane = 2,
        .row = 4,
        .description = "WorldComposer composes the World from the layer packs held, as USD composes a stage (sublayers, references, payloads, "
                       "instancing, LOD levels), and asks for the packs it names and lacks: one level of arcs deeper each round.",
        .records = {"scenethread", "scene", "lod"},
        .windows = {"sceneflow", "lod"},
        .readme = "src/world/README.md",
    },
    {
        .id = "scene.extract",
        .label = "Extract",
        .lane = 2,
        .row = 5,
        .description = "Walks the World and builds the renderer's inputs: a draw per prim submesh, lights, cameras and LOD roots. Meshes, "
                       "materials and textures are kept per asset in libraries across rebuilds.",
        .records = {"scenethread", "lod"},
        .windows = {"sceneflow", "lod"},
        .readme = "src/view/README.md",
    },
    {
        .id = "scene.changes",
        .label = "Change lists",
        .lane = 2,
        .row = 6,
        .description = "What an extract changed against the last one (draws, library entries, lights), published with a snapshot of the "
                       "scene for the main thread's queries.",
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
        .description = "SDL events: fly camera (WASD, mouse look, the wheel for speed), picking, gizmos, shortcuts. Scene cameras and "
                       "bookmarks place the camera.",
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
        .id = "render.framegraph",
        .label = "Frame graph",
        .lane = 4,
        .row = 9,
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
        .row = 10,
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
    {.from = "view.assetclient", .to = "scene.extract", .what = "mesh, material and texture packs"},
    {.from = "view.assetclient", .to = "render.pipelines", .what = "shaders (SPIR-V); changed ones reload"},
    {.from = "scene.compose", .to = "view.assetclient", .what = "asset.request for the packs it names"},
    {.from = "scene.compose", .to = "scene.extract", .what = "the World"},
    {.from = "scene.extract", .to = "scene.changes", .what = "draws, library entries, lights"},
    {.from = "main.rpc", .to = "scene.compose", .what = "requests: open, LOD levels, purposes, repack"},
    {.from = "scene.changes", .to = "main.scene", .what = "snapshot and change lists"},
    {.from = "main.scene", .to = "render.gpuscene", .what = "change lists (submitSceneChanges)"},
    {.from = "render.gpuscene", .to = "main.scene", .what = "drawn and idle: residency"},
    {.from = "main.input", .to = "main.frame", .what = "camera"},
    {.from = "main.ui", .to = "main.frame", .what = "settings, UI draw data"},
    {.from = "main.frame", .to = "render.framegraph", .what = "RenderSnapshot"},
    {.from = "render.gpuscene", .to = "render.uploads", .what = "queued copies"},
    {.from = "render.uploads", .to = "render.pool", .what = "vertices and indices"},
    {.from = "render.pool", .to = "render.framegraph", .what = "geometry and tables, read by culling and the passes"},
    {.from = "render.pipelines", .to = "render.framegraph", .what = "pipelines"},
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
