#include "viewdumps.h"

#include "gpuscenewindow.h"
#include "renderdebugjson.h"
#include "renderworld.h"
#include "trace.h"
#include "usdscene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <sstream>

namespace {

// Runs a FILE* writer into memory and parses what it wrote.
auto writerJson(const std::function<void(FILE*)>& write) -> std::optional<rpc::Json> {
    char* buffer = nullptr;
    size_t size = 0;
    FILE* f = open_memstream(&buffer, &size);
    if (f == nullptr) {
        return std::nullopt;
    }
    write(f);
    std::fclose(f);
    std::string text(buffer, size);
    std::free(buffer);
    auto json = rpc::Json::parse(text, nullptr, false);
    if (json.is_discarded()) {
        return std::nullopt;
    }
    return json;
}

// Answers target.responder with the writer's JSON, adding the file path when one was written.
auto respondWith(DumpTarget& target, const std::function<void(FILE*)>& write) -> void {
    if (!target.responder.valid()) {
        return;
    }
    auto json = writerJson(write);
    if (!json.has_value()) {
        target.responder.fail(rpc::internalError, "the dump did not produce valid JSON");
        return;
    }
    if (!target.path.empty() && json->is_object()) {
        (*json)["path"] = target.path;
    }
    target.responder.respond(*json);
}

// Answers a responder with the writer's JSON.
auto respondWithJson(RpcResponder& responder, const std::function<void(FILE*)>& write) -> void {
    auto json = writerJson(write);
    if (!json.has_value()) {
        responder.fail(rpc::internalError, "the dump did not produce valid JSON");
        return;
    }
    responder.respond(*json);
}

auto base64(const std::vector<char>& bytes) -> std::string {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t chunk = (uint32_t) (uint8_t) bytes[i] << 16;
        if (i + 1 < bytes.size()) {
            chunk |= (uint32_t) (uint8_t) bytes[i + 1] << 8;
        }
        if (i + 2 < bytes.size()) {
            chunk |= (uint32_t) (uint8_t) bytes[i + 2];
        }
        out += alphabet[(chunk >> 18) & 63];
        out += alphabet[(chunk >> 12) & 63];
        out += i + 1 < bytes.size() ? alphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? alphabet[chunk & 63] : '=';
    }
    return out;
}

} // namespace

ViewDumps::ViewDumps(const RenderWorld& world, const USDScene& scene, const CullResult& cull, bool statistics)
    : renderWorld(world), usdScene(scene), latestCull(cull), pipelineStatistics(statistics) {
}

auto ViewDumps::primPath(uint32_t prim) const -> std::string {
    const auto* rec = usdScene.isOpen() ? usdScene.getPrimRecord(PrimHandle{prim}) : nullptr;
    return rec != nullptr ? rec->path : std::string{};
}

auto ViewDumps::requestRenderDebug(RpcResponder responder) -> void {
    renderDebugTargets.push_back(std::move(responder));
}

auto ViewDumps::requestMemory(RpcResponder responder) -> void {
    memoryTargets.push_back(std::move(responder));
}

auto ViewDumps::requestCounters(RpcResponder responder) -> void {
    counterTargets.push_back(std::move(responder));
}

auto ViewDumps::requestCapture(CaptureWatch watch, DumpTarget target) -> void {
    watch.id = nextCaptureId++;
    captureDumps.push_back({.watch = std::move(watch), .target = std::move(target)});
}

auto ViewDumps::requestFrame(DumpTarget target) -> void {
    if (!target.path.empty()) {
        std::filesystem::create_directories(target.path);
    }
    frameRequests.push_back({.target = std::move(target)});
}

auto ViewDumps::requestGpuScene(DumpTarget target) -> void {
    if (!target.path.empty()) {
        std::filesystem::create_directories(target.path);
    }
    auto group = nextGpuSceneGroup++;
    auto dir = target.path;
    gpuSceneGroups[group] = {.target = std::move(target)};
    for (const auto& [pass, resource] : gpuSceneCaptures()) {
        CaptureDump dump = {
            .watch = {.id = nextCaptureId++, .pass = pass, .resource = resource, .trigger = 1},
            .target = {.path = dir.empty() ? std::string{} : std::format("{}/{}.json", dir, resource)},
            .gpuSceneGroup = group,
        };
        captureDumps.push_back(std::move(dump));
    }
}

auto ViewDumps::trackScreenshot(std::string path, RpcResponder responder, bool inlinePng) -> void {
    screenshots.push_back({.path = std::move(path), .responder = std::move(responder), .inlinePng = inlinePng});
}

auto ViewDumps::wantsFrameGraphDebug() const -> bool {
    return std::ranges::any_of(frameRequests, [](const FrameRequest& r) { return !r.armed && !r.target.path.empty(); });
}

auto ViewDumps::wantsRenderDebug() const -> bool {
    return !renderDebugTargets.empty() || !memoryTargets.empty();
}

auto ViewDumps::wantsCounters() const -> bool {
    return !counterTargets.empty();
}

auto ViewDumps::onFrameGraph(const std::optional<FrameGraphDebugSnapshot>& snapshot) -> void {
    if (snapshot.has_value()) {
        latestGraph = snapshot;
    }
    for (auto& request : frameRequests) {
        if (request.armed) {
            continue;
        }
        const auto& dir = request.target.path;
        if (dir.empty()) {
            // Only the frame debug capture was asked for; no per-pass captures.
            request.armed = true;
            frameDebugPending = true;
            continue;
        }
        if (!latestGraph.has_value()) {
            continue;
        }
        // Every resource each executed pass writes, captured right after that pass.
        for (uint32_t order = 0; order < latestGraph->executionOrder.size(); order++) {
            const auto& pass = latestGraph->passes[latestGraph->executionOrder[order]];
            if (pass.culled) {
                continue;
            }
            for (const auto& w : pass.writes) {
                const auto& res = latestGraph->resources[w.resourceIndex];
                auto base = std::format("{}/{:02}_{}_{}", dir, order, pass.name, res.name);
                auto path = base + (res.buffer ? ".json" : ".png");
                captureDumps.push_back({.watch = {.id = nextCaptureId++, .pass = pass.name, .resource = res.name, .trigger = 1}, .target = {.path = path}});
            }
        }
        request.armed = true;
        frameDebugPending = true;
    }
}

auto ViewDumps::appendWatches(std::vector<CaptureWatch>& watches) const -> void {
    for (const auto& dump : captureDumps) {
        watches.push_back(dump.watch);
    }
}

auto ViewDumps::takeFrameDebugRequest() -> bool {
    return std::exchange(frameDebugPending, false);
}

auto ViewDumps::onFrameDebug(const FrameDebugCapture& capture) -> void {
    std::vector<FrameRequest> waiting;
    for (auto& request : frameRequests) {
        if (!request.armed) {
            waiting.push_back(std::move(request));
            continue;
        }
        auto& target = request.target;
        if (!target.path.empty()) {
            auto path = target.path + "/frame.json";
            if (writeFrameDebugJson(path.c_str(), capture)) {
                TRACE_EVENT("Render", "FrameDebugWritten", path)
                    .text(std::format("frame debug written: {} (frame {}, {} passes)", path, capture.frame, capture.passes.size()))
                    .field("frame", (int64_t) capture.frame);
            } else {
                TRACE_ERROR("Render", "DumpFailed", path).text(std::format("dump-frame: cannot write {}", path));
            }
        }
        respondWith(target, [&](FILE* f) { writeFrameDebugJson(f, capture); });
    }
    frameRequests = std::move(waiting);
}

auto ViewDumps::onCaptureResult(CaptureResult& result) -> bool {
    auto dump = std::ranges::find_if(captureDumps, [&](const CaptureDump& d) { return d.watch.id == result.id; });
    if (dump == captureDumps.end()) {
        return false;
    }
    auto& target = dump->target;
    if (!result.error.empty()) {
        if (!target.path.empty() || dump->gpuSceneGroup >= 0 || !target.responder.valid()) {
            TRACE_ERROR("Render", "CaptureFailed", result.resource).text(std::format("capture {} {}: {}", result.pass, result.resource, result.error));
        }
        target.responder.fail(rpc::appError, result.error);
    } else {
        if (!target.path.empty()) {
            if (writeCaptureFiles(target.path, result, dump->watch.display, 65536)) {
                TRACE_EVENT("Render", "CaptureWritten", target.path)
                    .text(std::format("capture written: {} ({} {}, frame {})", target.path, result.pass.empty() ? "-" : result.pass, result.resource, result.frame))
                    .field("frame", (int64_t) result.frame);
            } else {
                TRACE_ERROR("Render", "DumpFailed", target.path).text(std::format("capture: cannot write {}", target.path));
            }
        }
        respondWith(target, [&](FILE* f) { writeCaptureJson(f, result, 65536); });
    }
    auto group = dump->gpuSceneGroup;
    captureDumps.erase(dump);
    if (group >= 0) {
        auto& g = gpuSceneGroups[group];
        g.results[result.resource] = std::move(result);
        auto stillPending = std::ranges::any_of(captureDumps, [group](const CaptureDump& d) { return d.gpuSceneGroup == group; });
        if (!stillPending) {
            finishGpuScene(group);
        }
    }
    return true;
}

auto ViewDumps::finishGpuScene(int group) -> void {
    auto& g = gpuSceneGroups[group];
    auto primOfInstance = primOfEachInstance(renderWorld);
    auto path = [&](uint32_t prim) -> std::string {
        auto p = primPath(prim);
        return p.empty() ? std::string("(unknown prim)") : p;
    };
    auto rows = buildGpuSceneRows(g.results, primOfInstance, path);
    auto viewCount = 1 + latestCull.cascadeCount;
    auto& target = g.target;
    if (!target.path.empty()) {
        auto file = target.path + "/instances_joined.json";
        if (writeGpuSceneJoinedJson(file, rows, viewCount)) {
            TRACE_EVENT("Render", "GpuSceneWritten", file).text(std::format("GPU scene written: {} ({} instances)", file, rows.size()));
        } else {
            TRACE_ERROR("Render", "DumpFailed", file).text(std::format("dump-gpuscene: cannot write {}", file));
        }
    }
    respondWith(target, [&](FILE* f) { writeGpuSceneJoinedJson(f, rows, viewCount); });
    gpuSceneGroups.erase(group);
}

auto ViewDumps::onCounters(const GpuCounters& counters) -> void {
    // Frames recorded before statistics were on have none; wait for one that has them.
    bool complete = !counters.passes.empty() || !pipelineStatistics;
    if (counterTargets.empty() || !complete) {
        return;
    }
    for (auto& responder : counterTargets) {
        respondWithJson(responder, [&](FILE* f) { writeGpuCountersJson(f, counters); });
    }
    counterTargets.clear();
}

auto ViewDumps::writeRenderDebug(const RenderDebugSnapshot& snapshot) -> void {
    auto path = [&](uint32_t prim) {
        return primPath(prim);
    };
    for (auto& responder : renderDebugTargets) {
        respondWithJson(responder, [&](FILE* f) { writeRenderDebugJson(f, snapshot, path); });
    }
    renderDebugTargets.clear();
}

auto ViewDumps::onRenderDebug(const std::optional<RenderDebugSnapshot>& snapshot) -> void {
    if (!snapshot.has_value()) {
        return;
    }
    // Render debug waits for a snapshot with the draw log in it.
    if (!renderDebugTargets.empty() && !snapshot->draws.empty()) {
        writeRenderDebug(*snapshot);
    }
    if (!memoryTargets.empty() && !snapshot->allocations.empty()) {
        for (auto& responder : memoryTargets) {
            respondWithJson(responder, [&](FILE* f) { writeMemoryJson(f, *snapshot); });
        }
        memoryTargets.clear();
    }
}

auto ViewDumps::onScreenshots(const std::vector<ScreenshotResult>& results) -> void {
    for (const auto& result : results) {
        auto pending = std::ranges::find_if(screenshots, [&](const PendingScreenshot& s) { return s.path == result.path; });
        if (pending == screenshots.end()) {
            continue;
        }
        if (!result.ok) {
            pending->responder.fail(rpc::appError, std::format("cannot write {}", result.path));
        } else {
            rpc::Json reply = {
                {"path", result.path},
                {"frame", result.frame},
                {"width", result.width},
                {"height", result.height},
            };
            if (pending->inlinePng) {
                std::ifstream in(result.path, std::ios::binary);
                std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                reply["png"] = base64(bytes);
            }
            pending->responder.respond(reply);
        }
        screenshots.erase(pending);
    }
}
