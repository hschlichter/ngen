#include "profilecommands.h"

#include "profile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <map>

namespace profile {

namespace {

using rpc::Json;

auto profileRecord() -> Json {
    std::vector<FrameStats> history;
    frameHistory(history);
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

    std::vector<LaneUsage> usage;
    historyUsage(usage);
    auto lanes = Json::array();
    uint64_t historyBytes = 0;
    for (const auto& lane : usage) {
        lanes.push_back({{"lane", lane.name}, {"zones", lane.zones}, {"bytes", lane.bytes}});
        historyBytes += lane.bytes;
    }
    auto driverBusy = gpuDriverBusyPercent();
    return {
        {"frameCount", history.size()},
        {"cpuMsAverage", cpuAverage},
        {"gpuMsAverage", gpuAverage},
        {"gpuBusyPercent", gpuBusyPercent()},
        {"gpuBusyDriverPercent", driverBusy ? Json(*driverBusy) : Json(nullptr)},
        {"gpuBusyDriverSource", gpuDriverBusyStatus()},
        {"residentBytes", residentBytes()},
        {"historySeconds", historySeconds()},
        {"historyBytes", historyBytes},
        {"gpuDetail", gpuDetail()},
        {"lanes", lanes},
        {"frames", frames},
    };
}

// Where the time goes: every zone over the last 60 frames (a process without frames: the whole history), per thread lane and the GPU's,
// by site: how often it ran, its total, self and longest time, the share its children cover, and how deep it nests, the most expensive first.
auto zonesRecord() -> Json {
    constexpr uint64_t windowFrames = 60;
    uint64_t frames = 0;
    uint64_t startNs = 0;
    uint64_t endNs = now();
    if (auto latest = lastFrame()) {
        frames = 1;
        auto first = *latest;
        for (uint64_t offset = 1; offset < windowFrames; offset++) {
            auto frame = frameAt(offset);
            if (!frame) {
                break;
            }
            first = *frame;
            frames++;
        }
        startNs = first.startNs;
        endNs = latest->endNs;
    } else {
        auto span = (uint64_t) (historySeconds() * 1e9);
        startNs = endNs > span ? endNs - span : 0;
    }
    struct Totals {
        uint64_t count = 0;
        uint64_t totalNs = 0;
        uint64_t childNs = 0;
        uint64_t maxNs = 0;
        uint16_t depth = UINT16_MAX;
        std::vector<uint64_t> durations;
    };
    auto summarise = [&](std::vector<Zone>& zones) {
        // Each zone's children by nesting: in start order, a zone's parent is the innermost zone still open when it starts.
        std::ranges::sort(zones, [](const Zone& a, const Zone& b) {
            if (a.startNs != b.startNs) {
                return a.startNs < b.startNs;
            }
            return a.depth < b.depth;
        });
        std::vector<uint64_t> childNs(zones.size(), 0);
        std::vector<size_t> open;
        for (size_t i = 0; i < zones.size(); i++) {
            while (!open.empty() && zones[open.back()].endNs <= zones[i].startNs) {
                open.pop_back();
            }
            if (!open.empty()) {
                childNs[open.back()] += zones[i].endNs - zones[i].startNs;
            }
            open.push_back(i);
        }
        std::map<uint32_t, Totals> bySite;
        for (size_t i = 0; i < zones.size(); i++) {
            const auto& zone = zones[i];
            auto& totals = bySite[zone.siteId];
            auto duration = zone.endNs - zone.startNs;
            totals.count++;
            totals.totalNs += duration;
            totals.childNs += std::min(childNs[i], duration);
            totals.maxNs = std::max(totals.maxNs, duration);
            totals.depth = std::min(totals.depth, zone.depth);
            totals.durations.push_back(duration);
        }
        std::vector<std::pair<uint32_t, Totals>> sorted(bySite.begin(), bySite.end());
        std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.totalNs > b.second.totalNs; });
        auto out = Json::array();
        for (auto& [siteId, totals] : sorted) {
            // The median run, beside the total: one long run (a hitch) and a steady cost read differently.
            std::ranges::nth_element(totals.durations, totals.durations.begin() + (std::ptrdiff_t) (totals.durations.size() / 2));
            auto medianNs = totals.durations[totals.durations.size() / 2];
            auto site = siteOf(siteId);
            Json zone = {
                {"name", site.name},
                {"group", groupName(site.group)},
                {"level", site.level == Level::Detail ? "detail" : "guide"},
                {"file", site.file},
                {"line", site.line},
                {"function", site.function},
                {"depth", totals.depth},
                {"count", totals.count},
                {"totalMs", (double) totals.totalNs / 1e6},
                {"selfMs", (double) (totals.totalNs - totals.childNs) / 1e6},
                {"coveredShare", totals.totalNs == 0 ? 0.0 : (double) totals.childNs / (double) totals.totalNs},
                {"medianMs", (double) medianNs / 1e6},
                {"maxMs", (double) totals.maxNs / 1e6},
            };
            if (frames > 0) {
                zone["perFrameMs"] = (double) totals.totalNs / 1e6 / (double) frames;
            }
            out.push_back(std::move(zone));
        }
        return out;
    };
    std::vector<LaneInfo> laneInfos;
    lanes(laneInfos);
    auto lanesJson = Json::array();
    std::vector<Zone> zones;
    for (const auto& lane : laneInfos) {
        zonesIn(lane.index, startNs, endNs, zones);
        lanesJson.push_back({{"lane", lane.name}, {"zones", summarise(zones)}});
    }
    gpuZonesIn(startNs, endNs, zones);
    lanesJson.push_back({{"lane", "GPU"}, {"zones", summarise(zones)}});
    return {
        {"frames", frames},
        {"windowMs", (double) (endNs - startNs) / 1e6},
        {"lanes", lanesJson},
    };
}

auto formatName(PlotFormat format) -> const char* {
    switch (format) {
        case PlotFormat::Bytes:
            return "bytes";
        case PlotFormat::Percent:
            return "percent";
        case PlotFormat::Number:
            return "number";
    }
    return "number";
}

auto plotsRecord() -> Json {
    std::vector<PlotInfo> list;
    plots(list);
    auto out = Json::array();
    for (const auto& info : list) {
        out.push_back({
            {"name", info.name},
            {"group", groupName(info.group)},
            {"format", formatName(info.format)},
            {"samples", info.samples},
            {"latest", info.latest},
            {"min", info.min},
            {"average", info.average},
            {"max", info.max},
        });
    }
    return {{"historySeconds", historySeconds()}, {"plots", out}};
}

} // namespace

auto registerProfileRecords(RpcRecords& records) -> void {
    records.add("profile", "the profiler: CPU and GPU time per frame over the history, the history window and the zones and bytes each lane holds", [](RpcResponder responder) {
        responder.respond(profileRecord());
    });
    records.add("zones",
                "where the time goes over the last 60 frames (without frames: the whole history): every profiler zone site per thread lane and on the GPU, "
                "its file, line, group and level, count, total, self time and the share its children cover, per frame, median and longest time",
                [](RpcResponder responder) {
                    responder.respond(zonesRecord());
                });
    records.add("plots", "every profiler plot (renderer stats, culling, GPU usage, memory, uploads, assets) over the history window: group, format, samples, latest, min, average, max; introspect.plot {name} gives one plot's samples", [](RpcResponder responder) {
        responder.respond(plotsRecord());
    });
}

auto plotSamples(const rpc::Json& params) -> std::expected<rpc::Json, std::string> {
    if (!params.contains("name") || !params["name"].is_string()) {
        return std::unexpected("name: a string is required");
    }
    auto name = params["name"].get<std::string>();
    auto id = plotIdOf(name);
    if (!id) {
        return std::unexpected(std::format("no plot named '{}' (the plots record lists them)", name));
    }
    auto t = now();
    uint64_t startNs = 0;
    if (params.contains("seconds")) {
        if (!params["seconds"].is_number()) {
            return std::unexpected("seconds: a number");
        }
        auto span = (uint64_t) (params["seconds"].get<double>() * 1e9);
        startNs = t > span ? t - span : 0;
    }
    std::vector<PlotSample> samples;
    plotSamplesIn(*id, startNs, t, samples);
    std::vector<PlotInfo> list;
    plots(list);
    const auto& info = list[*id];
    auto out = Json::array();
    for (const auto& sample : samples) {
        out.push_back({{"ms", -(double) (t - sample.timeNs) * 1e-6}, {"value", sample.value}});
    }
    return Json{{"name", info.name}, {"group", groupName(info.group)}, {"format", formatName(info.format)}, {"samples", out}};
}

auto dumpProfile(const rpc::Json& params) -> std::expected<rpc::Json, std::string> {
    if (!params.contains("path") || !params["path"].is_string()) {
        return std::unexpected("path: a string is required");
    }
    auto path = params["path"].get<std::string>();
    ExportOptions options;
    if (params.contains("seconds")) {
        if (!params["seconds"].is_number()) {
            return std::unexpected("seconds: a number");
        }
        options.seconds = params["seconds"].get<double>();
    }
    if (params.contains("level")) {
        auto level = params["level"].is_string() ? params["level"].get<std::string>() : std::string();
        if (level == "guide") {
            options.detail = false;
        } else if (level != "detail") {
            return std::unexpected("level: guide or detail");
        }
    }
    if (!exportChromeTrace(path.c_str(), options)) {
        return std::unexpected(std::format("cannot write {}", path));
    }
    return Json{{"path", path}};
}

} // namespace profile
