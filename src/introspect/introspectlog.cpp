#include "introspectlog.h"

#include "introspecttarget.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <expected>
#include <optional>
#include <print>
#include <set>
#include <string_view>
#include <thread>

namespace {

constexpr int exitOk = 0;
constexpr int exitUsage = 3;

// How long an event waits after it arrived before it is written, so batches from other processes covering the same
// time have come in and the merge by time holds. Batches are sent every 50 ms.
constexpr std::chrono::milliseconds reorderDelay{250};

std::atomic<bool> stopRequested{false};

auto onSignal(int) -> void {
    stopRequested.store(true);
}

struct LogOptions {
    std::string output;
    std::string untilExit;
    std::vector<std::string> processes;
    std::set<std::string> categories;
    std::set<std::string> types;
    int minSeverity = 0; // severityRank: 0 info, 1 warning, 2 error
    bool history = false;
};

// Severities in order, so --severity=warning keeps warnings and errors.
auto severityRank(std::string_view severity) -> int {
    if (severity == "error") {
        return 2;
    }
    if (severity == "warning") {
        return 1;
    }
    return 0;
}

auto splitList(std::string_view text) -> std::vector<std::string> {
    std::vector<std::string> items;
    size_t start = 0;
    while (start <= text.size()) {
        auto comma = text.find(',', start);
        if (comma == std::string_view::npos) {
            comma = text.size();
        }
        if (comma > start) {
            items.emplace_back(text.substr(start, comma - start));
        }
        start = comma + 1;
    }
    return items;
}

auto parseOptions(std::span<const std::string> args) -> std::expected<LogOptions, std::string> {
    LogOptions options;
    auto value = [](std::string_view arg, std::string_view flag) {
        return arg.substr(flag.size());
    };
    for (const auto& arg : args) {
        std::string_view text = arg;
        if (text.starts_with("--output=")) {
            options.output = std::string(value(text, "--output="));
        } else if (text.starts_with("--until-exit=")) {
            options.untilExit = std::string(value(text, "--until-exit="));
        } else if (text.starts_with("--process=")) {
            options.processes = splitList(value(text, "--process="));
        } else if (text.starts_with("--category=")) {
            for (auto& item : splitList(value(text, "--category="))) {
                options.categories.insert(std::move(item));
            }
        } else if (text.starts_with("--type=")) {
            for (auto& item : splitList(value(text, "--type="))) {
                options.types.insert(std::move(item));
            }
        } else if (text.starts_with("--severity=")) {
            auto severity = value(text, "--severity=");
            if (severity != "info" && severity != "warning" && severity != "error") {
                return std::unexpected("--severity takes info, warning or error, not '" + std::string(severity) + "'");
            }
            options.minSeverity = severityRank(severity);
        } else if (text == "--history") {
            options.history = true;
        } else {
            return std::unexpected("unknown log argument '" + arg + "'");
        }
    }
    return options;
}

auto passes(const LogOptions& options, const rpc::Json& event) -> bool {
    if (!options.categories.empty() && !options.categories.contains(event.value("category", ""))) {
        return false;
    }
    if (!options.types.empty() && !options.types.contains(event.value("type", ""))) {
        return false;
    }
    return severityRank(event.value("severity", "info")) >= options.minSeverity;
}

struct Arrived {
    std::chrono::steady_clock::time_point at;
    IntrospectSession::LogEvent event;
};

} // namespace

auto logEventLine(const IntrospectSession::LogEvent& event) -> std::string {
    const auto& e = *event.event;
    nlohmann::ordered_json line;
    line["ts_ns"] = e.value("ts_ns", (uint64_t) 0);
    line["process"] = event.process;
    line["thread"] = e.value("thread", "");
    line["severity"] = e.value("severity", "info");
    line["category"] = e.value("category", "");
    line["type"] = e.value("type", "");
    line["name"] = e.value("name", "");
    line["text"] = e.value("text", "");
    line["fields"] = e.contains("fields") ? e["fields"] : rpc::Json::object();
    return line.dump();
}

auto runLogCommand(std::span<const std::string> args) -> int {
    auto parsed = parseOptions(args);
    if (!parsed) {
        std::println(stderr, "ngen-introspect: {}", parsed.error());
        return exitUsage;
    }
    const auto& options = *parsed;
    FILE* out = stdout;
    if (!options.output.empty()) {
        out = std::fopen(options.output.c_str(), "w");
        if (out == nullptr) {
            std::println(stderr, "ngen-introspect: cannot write {}", options.output);
            return exitUsage;
        }
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    auto startNs = (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    IntrospectSession session({.log = true, .logSinceNs = options.history ? 0 : startNs, .processes = options.processes});

    std::vector<Arrived> waiting;
    std::vector<IntrospectSession::LogEvent> taken;
    // With --output the whole run is sorted once at the end: a process that connects late brings history older than
    // what was already merged. Kept as finished lines.
    std::vector<std::pair<uint64_t, std::string>> collected;
    // Once set, the run ends after the reorder delay, so the target's last batches are in.
    std::optional<std::chrono::steady_clock::time_point> endAt;
    uint64_t written = 0;

    auto writeDue = [&](std::chrono::steady_clock::time_point cutoff) {
        auto due = std::ranges::partition(waiting, [&](const Arrived& a) { return a.at > cutoff; });
        std::vector<Arrived> ready(std::make_move_iterator(due.begin()), std::make_move_iterator(due.end()));
        waiting.erase(due.begin(), due.end());
        std::ranges::stable_sort(ready, [](const Arrived& a, const Arrived& b) { return a.event.tsNs < b.event.tsNs; });
        for (const auto& item : ready) {
            auto line = logEventLine(item.event);
            if (!options.output.empty()) {
                collected.emplace_back(item.event.tsNs, std::move(line));
                continue;
            }
            std::fwrite(line.data(), 1, line.size(), out);
            std::fputc('\n', out);
            written++;
        }
        std::fflush(out);
    };

    while (true) {
        session.poll();
        taken.clear();
        session.takeLogEvents(taken);
        auto now = std::chrono::steady_clock::now();
        for (auto& event : taken) {
            if (passes(options, *event.event)) {
                waiting.push_back({.at = now, .event = std::move(event)});
            }
        }
        if (!endAt && !options.untilExit.empty()) {
            // The target has exited once a matching process was connected and is no longer.
            auto live = session.processes();
            for (const auto& name : session.everConnected()) {
                auto colon = name.find(':');
                RpcEndpointInfo info;
                info.kind = name.substr(0, colon);
                info.pid = std::stoi(name.substr(colon + 1));
                bool alive = std::ranges::any_of(live, [&](const IntrospectSession::Process& p) { return p.name == name; });
                if (matchesTarget(options.untilExit, info) && !alive) {
                    endAt = now + reorderDelay;
                }
            }
        }
        if (!endAt && stopRequested.load()) {
            endAt = now + reorderDelay;
        }
        if (endAt && now >= *endAt) {
            break;
        }
        writeDue(now - reorderDelay);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    taken.clear();
    session.takeLogEvents(taken);
    for (auto& event : taken) {
        if (passes(options, *event.event)) {
            waiting.push_back({.at = std::chrono::steady_clock::now(), .event = std::move(event)});
        }
    }
    writeDue(std::chrono::steady_clock::time_point::max());
    std::ranges::stable_sort(collected, [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [ts, line] : collected) {
        std::fwrite(line.data(), 1, line.size(), out);
        std::fputc('\n', out);
        written++;
    }
    if (out != stdout) {
        std::fclose(out);
    }
    for (const auto& [process, count] : session.logDropped()) {
        std::println(stderr, "ngen-introspect: {} dropped {} events (overwritten before they were sent)", process, count);
    }
    if (!options.output.empty()) {
        std::println(stderr, "ngen-introspect: {} events written to {}", written, options.output);
    }
    return exitOk;
}
