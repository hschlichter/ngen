#pragma once

#include "capture.h"
#include "drawlists.h"
#include "framedebug.h"
#include "framegraphdebug.h"
#include "gpucounters.h"
#include "renderdebug.h"
#include "rpcresponder.h"
#include "screenshot.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

class USDScene;
struct RenderWorld;

// Where a dump goes: a file (script verbs, or an RPC call that names a path), an RPC reply,
// or both.
struct DumpTarget {
    std::string path;
    RpcResponder responder;
};

// The view's dumps and captures, which finish frames after they are asked for: render debug,
// memory, counters, captures, the frame debug capture, the GPU scene tables and screenshots.
// The main loop feeds it what the render thread delivers; each request completes its target
// when its data arrives. File output and console messages are the same as the old verbs'.
class ViewDumps {
public:
    ViewDumps(const RenderWorld& renderWorld, const USDScene& usdScene, const CullResult& latestCull, bool pipelineStatistics);

    // Render debug, memory and counters answer a responder only; they are records (introspect.get), not files.
    auto requestRenderDebug(RpcResponder responder) -> void;
    auto requestMemory(RpcResponder responder) -> void;
    auto requestCounters(RpcResponder responder) -> void;
    // `watch.id` is assigned here.
    auto requestCapture(CaptureWatch watch, DumpTarget target) -> void;
    // target.path is a directory: frame.json plus every written resource after every pass.
    // Without one, only the frame debug capture is answered.
    auto requestFrame(DumpTarget target) -> void;
    // target.path is a directory: the tables and instances_joined.json. Without one, only the
    // joined rows are answered.
    auto requestGpuScene(DumpTarget target) -> void;
    // A screenshot the renderer was asked for; answered when the file is written.
    auto trackScreenshot(std::string path, RpcResponder responder, bool inlinePng) -> void;

    // Per frame, from the main loop.
    auto wantsFrameGraphDebug() const -> bool;
    auto wantsRenderDebug() const -> bool;
    auto wantsCounters() const -> bool;
    auto onFrameGraph(const std::optional<FrameGraphDebugSnapshot>& snapshot) -> void;
    auto appendWatches(std::vector<CaptureWatch>& watches) const -> void;
    auto takeFrameDebugRequest() -> bool;
    auto onFrameDebug(const FrameDebugCapture& capture) -> void;
    // True when the result belonged to a dump; false leaves it for the editor's windows.
    auto onCaptureResult(CaptureResult& result) -> bool;
    auto onCounters(const GpuCounters& counters) -> void;
    auto onRenderDebug(const std::optional<RenderDebugSnapshot>& snapshot) -> void;
    auto onScreenshots(const std::vector<ScreenshotResult>& results) -> void;

private:
    struct CaptureDump {
        CaptureWatch watch;
        DumpTarget target;
        int gpuSceneGroup = -1; // part of a GPU scene dump, joined when the group is complete
    };
    struct GpuSceneGroup {
        DumpTarget target;
        std::map<std::string, CaptureResult> results;
    };
    struct FrameRequest {
        DumpTarget target;
        bool armed = false; // captures and the frame debug capture requested
    };
    struct PendingScreenshot {
        std::string path;
        RpcResponder responder;
        bool inlinePng = false;
    };

    auto primPath(uint32_t prim) const -> std::string;
    auto writeRenderDebug(const RenderDebugSnapshot& snapshot) -> void;
    auto finishGpuScene(int group) -> void;

    const RenderWorld& renderWorld;
    const USDScene& usdScene;
    const CullResult& latestCull;
    bool pipelineStatistics = false;

    std::vector<RpcResponder> renderDebugTargets;
    std::vector<RpcResponder> memoryTargets;
    std::vector<RpcResponder> counterTargets;
    std::vector<CaptureDump> captureDumps;
    uint32_t nextCaptureId = 100000;
    std::map<int, GpuSceneGroup> gpuSceneGroups;
    int nextGpuSceneGroup = 0;
    std::vector<FrameRequest> frameRequests;
    bool frameDebugPending = false;
    std::optional<FrameGraphDebugSnapshot> latestGraph;
    std::vector<PendingScreenshot> screenshots;
};
