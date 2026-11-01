#include "viewcommands.h"

#include "profile.h"
#include "trace.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <expected>
#include <format>
#include <map>

// The profiler's commands: its history written as a Chrome trace, and the records of the frame history and where the time goes.

namespace {

using rpc::Json;

auto parsePath(const std::string& args) -> std::expected<Json, std::string> {
    return Json{{"path", args}};
}

auto profileRecord() -> Json {
    std::vector<profile::FrameStats> history;
    profile::frameHistory(history);
    auto frames = Json::array();
    double cpuSum = 0.0;
    double gpuSum = 0.0;
    size_t gpuCount = 0;
    for (const auto& frame : history) {
        frames.push_back({{"frame", frame.frameIndex}, {"cpuMs", frame.cpuMs}, {"gpuMs", frame.gpuMs}});
        cpuSum += frame.cpuMs;
        if (frame.gpuMs > 0.0) {
            gpuSum += frame.gpuMs;
            gpuCount++;
        }
    }
    double cpuAverage = history.empty() ? 0.0 : cpuSum / (double) history.size();
    double gpuAverage = gpuCount == 0 ? 0.0 : gpuSum / (double) gpuCount;
    return {
        {"frameCount", history.size()},
        {"cpuMsAverage", cpuAverage},
        {"gpuMsAverage", gpuAverage},
        {"frames", frames},
    };
}

// Where the time goes: every zone over the last frames, per thread lane (and the GPU's), by name: how often it ran, its total and longest
// time, and how deep it nests, the most expensive first.
auto zonesRecord() -> Json {
    constexpr uint64_t windowFrames = 60;
    auto latest = profile::lastFrame();
    if (!latest) {
        return {{"frames", 0}, {"lanes", Json::array()}};
    }
    uint64_t frames = 1;
    auto first = *latest;
    for (uint64_t offset = 1; offset < windowFrames; offset++) {
        auto frame = profile::frameAt(offset);
        if (!frame) {
            break;
        }
        first = *frame;
        frames++;
    }
    struct Totals {
        uint64_t count = 0;
        uint64_t totalNs = 0;
        uint64_t maxNs = 0;
        uint16_t depth = UINT16_MAX;
        std::vector<uint64_t> durations;
    };
    auto summarise = [&](const std::vector<profile::Zone>& zones) {
        std::map<uint32_t, Totals> byName;
        for (const auto& zone : zones) {
            auto& totals = byName[zone.nameId];
            auto duration = zone.endNs - zone.startNs;
            totals.count++;
            totals.totalNs += duration;
            totals.maxNs = std::max(totals.maxNs, duration);
            totals.depth = std::min(totals.depth, zone.depth);
            totals.durations.push_back(duration);
        }
        std::vector<std::pair<uint32_t, Totals>> sorted(byName.begin(), byName.end());
        std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.totalNs > b.second.totalNs; });
        auto out = Json::array();
        for (auto& [nameId, totals] : sorted) {
            // The median run, beside the total: one long run (a hitch) and a steady cost read differently.
            std::ranges::nth_element(totals.durations, totals.durations.begin() + (std::ptrdiff_t) (totals.durations.size() / 2));
            auto medianNs = totals.durations[totals.durations.size() / 2];
            out.push_back({
                {"medianMs", (double) medianNs / 1e6},
                {"name", profile::nameOf(nameId)},
                {"depth", totals.depth},
                {"count", totals.count},
                {"totalMs", (double) totals.totalNs / 1e6},
                {"perFrameMs", (double) totals.totalNs / 1e6 / (double) frames},
                {"maxMs", (double) totals.maxNs / 1e6},
            });
        }
        return out;
    };
    std::vector<profile::LaneInfo> laneInfos;
    profile::lanes(laneInfos);
    auto lanes = Json::array();
    std::vector<profile::Zone> zones;
    for (const auto& lane : laneInfos) {
        zones.clear();
        profile::zonesIn(lane.index, first.startNs, latest->endNs, zones);
        lanes.push_back({{"lane", lane.name}, {"zones", summarise(zones)}});
    }
    zones.clear();
    profile::gpuZonesIn(first.startNs, latest->endNs, zones);
    lanes.push_back({{"lane", "GPU"}, {"zones", summarise(zones)}});
    return {
        {"frames", frames},
        {"windowMs", (double) (latest->endNs - first.startNs) / 1e6},
        {"lanes", lanes},
    };
}

} // namespace

auto registerProfileCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records) -> void {
    registry.add(
        {
            .name = "introspect.profile",
            .summary = "Write the profiler history as a Chrome trace.",
            .params = {requiredField("path", RpcType::String, "trace JSON path")},
            .result = "{path}",
        },
        [](const Json& p, RpcResponder responder) {
            auto path = p["path"].get<std::string>();
            if (!profile::exportChromeTrace(path.c_str())) {
                responder.fail(rpc::appError, std::format("cannot write {}", path));
                return;
            }
            TRACE_EVENT("Engine", "ProfileWritten", path).text(std::format("profile trace written: {}", path));
            responder.respond({{"path", path}});
        });

    verbs.add("dump-profile", "introspect.profile", parsePath);

    records.add("profile", "the profiler's frame history: CPU and GPU time per frame", [](RpcResponder responder) {
        responder.respond(profileRecord());
    });
    records.add("zones", "where the time goes over the last 60 frames: every profiler zone per thread lane and on the GPU, its count, total, per frame, median and longest time", [](RpcResponder responder) {
        responder.respond(zonesRecord());
    });
}
