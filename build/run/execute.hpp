#pragma once

#include "../framework/glob.hpp"
#include "../ir/schema.hpp"
#include "../framework/path.hpp"
#include "buildlog.hpp"
#include "depfile.hpp"
#include "hash.hpp"
#include "process.hpp"
#include "progress.hpp"
#include "scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ngen::run {

struct RunOptions {
    int jobs = 0; // 0 = let main fill in from hardware_concurrency
    int keep_going = 1;
    bool verbose = false;
    bool very_verbose = false;
    std::vector<std::string> targets; // empty = use IR's default_targets.
    build::Path ir_path;              // used to derive the build log path
};

struct RunResult {
    std::size_t total_edges = 0;
    std::size_t executed = 0;
    std::size_t skipped = 0;
    std::size_t failures = 0;
    bool interrupted = false;
};

namespace detail {

inline auto build_output_index(const build::ir::IR& ir) -> std::unordered_map<std::string, std::uint32_t> {
    std::unordered_map<std::string, std::uint32_t> out;
    for (std::uint32_t i = 0; i < ir.edges.size(); ++i) {
        for (const auto& output : ir.edges[i].outputs) {
            out.emplace(output, i);
        }
    }
    return out;
}

inline auto build_name_index(const build::ir::IR& ir) -> std::unordered_map<std::string, std::uint32_t> {
    std::unordered_map<std::string, std::uint32_t> out;
    for (std::uint32_t i = 0; i < ir.edges.size(); ++i) {
        out.emplace(ir.edges[i].name, i);
    }
    return out;
}

inline auto resolve_target(const std::string& target, const std::unordered_map<std::string, std::uint32_t>& by_name,
                           const std::unordered_map<std::string, std::uint32_t>& by_output) -> std::optional<std::uint32_t> {
    if (auto it = by_name.find(target); it != by_name.end()) {
        return it->second;
    }
    if (auto it = by_output.find(target); it != by_output.end()) {
        return it->second;
    }
    return std::nullopt;
}

inline auto collect_reachable(const build::ir::IR& ir, std::uint32_t root_edge, const std::unordered_map<std::string, std::uint32_t>& by_output,
                              std::unordered_set<std::uint32_t>& out) -> void {
    if (!out.insert(root_edge).second) {
        return;
    }
    const auto& edge = ir.edges[root_edge];
    auto follow = [&](const std::vector<std::string>& paths) {
        for (const auto& p : paths) {
            if (auto it = by_output.find(p); it != by_output.end()) {
                collect_reachable(ir, it->second, by_output, out);
            }
        }
    };
    follow(edge.inputs);
    follow(edge.implicit_deps);
    follow(edge.order_only_deps);
}

inline auto find_tracked(const std::vector<TrackedFile>& list, const std::string& path) -> const TrackedFile* {
    for (const auto& t : list) {
        if (t.path == path) {
            return &t;
        }
    }
    return nullptr;
}

// Refresh a list of paths into TrackedFiles, reusing prior content_hash via the
// mtime fast-path. Returns false in `any_missing` if any path is absent on disk.
inline auto refresh_list(const std::vector<std::string>& paths, const std::vector<TrackedFile>& previous, std::vector<TrackedFile>& out,
                         bool& any_missing) -> std::expected<void, build::Error> {
    out.clear();
    out.reserve(paths.size());
    for (const auto& p : paths) {
        TrackedFile t;
        t.path = p;
        const TrackedFile* prev = find_tracked(previous, p);
        bool present = false;
        auto r = refresh(t, prev, present);
        if (!r) {
            return std::unexpected(r.error());
        }
        if (!present) {
            any_missing = true;
        }
        out.push_back(std::move(t));
    }
    return {};
}

inline auto now_ns() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

struct DirtyCheck {
    LogEntry entry; // populated with current on-disk state (pre-run)
    bool dirty = false;
};

inline auto compute_dirty(const build::ir::Edge& edge, const LogEntry* prev) -> std::expected<DirtyCheck, build::Error> {
    DirtyCheck out;
    out.entry.command_hash = hash_string(edge.command);
    if (!prev || prev->command_hash != out.entry.command_hash) {
        out.dirty = true;
    }

    bool input_missing = false;
    if (auto r = refresh_list(edge.inputs, prev ? prev->inputs : std::vector<TrackedFile>{}, out.entry.inputs, input_missing); !r) {
        return std::unexpected(r.error());
    }
    if (input_missing) {
        out.dirty = true;
    }
    if (prev) {
        for (const auto& t : out.entry.inputs) {
            const TrackedFile* prev_t = find_tracked(prev->inputs, t.path);
            if (!prev_t || prev_t->content_hash != t.content_hash) {
                out.dirty = true;
            }
        }
    } else {
        out.dirty = true;
    }

    // implicit_deps tracked alongside inputs (we don't distinguish here — same dirty semantics).
    std::vector<TrackedFile> implicit_tracked;
    bool implicit_missing = false;
    if (auto r = refresh_list(edge.implicit_deps, prev ? prev->inputs : std::vector<TrackedFile>{}, implicit_tracked, implicit_missing); !r) {
        return std::unexpected(r.error());
    }
    if (implicit_missing) {
        out.dirty = true;
    }
    for (auto& t : implicit_tracked) {
        if (prev) {
            const TrackedFile* prev_t = find_tracked(prev->inputs, t.path);
            if (!prev_t || prev_t->content_hash != t.content_hash) {
                out.dirty = true;
            }
        }
        out.entry.inputs.push_back(std::move(t));
    }

    // discovered_headers carried over from the last successful run.
    if (prev) {
        std::vector<std::string> header_paths;
        header_paths.reserve(prev->discovered_headers.size());
        for (const auto& h : prev->discovered_headers) {
            header_paths.push_back(h.path);
        }
        bool header_missing = false;
        if (auto r = refresh_list(header_paths, prev->discovered_headers, out.entry.discovered_headers, header_missing); !r) {
            return std::unexpected(r.error());
        }
        if (header_missing) {
            out.dirty = true;
        }
        for (const auto& h : out.entry.discovered_headers) {
            const TrackedFile* prev_h = find_tracked(prev->discovered_headers, h.path);
            if (!prev_h || prev_h->content_hash != h.content_hash) {
                out.dirty = true;
            }
        }
    }

    // Outputs. Missing or hash-mismatched output = dirty. Phony edges have no
    // command and their "outputs" are virtual stamp paths that never exist on
    // disk; skip the existence check for them.
    bool is_phony = (edge.flags & build::ir::kEdgeFlagPhony) != 0;
    if (!is_phony) {
        bool output_missing = false;
        if (auto r = refresh_list(edge.outputs, prev ? prev->outputs : std::vector<TrackedFile>{}, out.entry.outputs, output_missing); !r) {
            return std::unexpected(r.error());
        }
        if (output_missing) {
            out.dirty = true;
        }
        if (prev) {
            for (const auto& t : out.entry.outputs) {
                const TrackedFile* prev_t = find_tracked(prev->outputs, t.path);
                if (!prev_t || prev_t->content_hash != t.content_hash) {
                    out.dirty = true;
                }
            }
        }
    }

    return out;
}

inline auto buildlog_path(const build::Path& ir_path) -> build::Path { return ir_path.parent_path() / build::Path(".ngen-buildlog"); }

} // namespace detail

inline auto execute(const build::ir::IR& ir, const RunOptions& opts) -> std::expected<RunResult, build::Error> {
    using namespace detail;

    auto by_output = build_output_index(ir);
    auto by_name = build_name_index(ir);

    std::vector<std::uint32_t> roots;
    if (opts.targets.empty()) {
        roots = ir.default_targets;
        if (roots.empty()) {
            return std::unexpected(build::Error{"IR has no default targets and no targets were provided"});
        }
    } else {
        roots.reserve(opts.targets.size());
        for (const auto& t : opts.targets) {
            auto idx = resolve_target(t, by_name, by_output);
            if (!idx) {
                return std::unexpected(build::Error{"unknown target: " + t});
            }
            roots.push_back(*idx);
        }
    }

    std::unordered_set<std::uint32_t> reachable;
    for (auto r : roots) {
        collect_reachable(ir, r, by_output, reachable);
    }

    // Build reachable-set in IR (topological) order.
    std::vector<std::uint32_t> ordered;
    ordered.reserve(reachable.size());
    for (std::uint32_t i = 0; i < ir.edges.size(); ++i) {
        if (reachable.contains(i)) {
            ordered.push_back(i);
        }
    }

    // Load the per-variant build log.
    BuildLog log;
    auto log_path = buildlog_path(opts.ir_path);
    if (auto r = log.load(log_path); !r) {
        std::cerr << "warning: " << r.error().message << " (continuing as if log were empty)\n";
    }

    // Local dirty check + propagation along dependents.
    std::vector<DirtyCheck> checks(ordered.size());
    std::unordered_map<std::uint32_t, std::size_t> pos_in_ordered;
    pos_in_ordered.reserve(ordered.size());
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        pos_in_ordered[ordered[i]] = i;
    }

    std::vector<bool> dirty(ordered.size(), false);
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        const auto& edge = ir.edges[ordered[i]];
        const LogEntry* prev = log.find(edge.name);
        auto check = compute_dirty(edge, prev);
        if (!check) {
            return std::unexpected(check.error());
        }
        checks[i] = std::move(*check);
        dirty[i] = checks[i].dirty;
        if (!dirty[i]) {
            // If any dep is dirty, this edge is dirty too — running deps will mutate its inputs.
            for (const auto& list : {edge.inputs, edge.implicit_deps}) {
                for (const auto& p : list) {
                    auto it = by_output.find(p);
                    if (it != by_output.end()) {
                        auto pos_it = pos_in_ordered.find(it->second);
                        if (pos_it != pos_in_ordered.end() && dirty[pos_it->second]) {
                            dirty[i] = true;
                            break;
                        }
                    }
                }
                if (dirty[i]) {
                    break;
                }
            }
        }
    }

    // Build the plan from the dirty subset.
    std::vector<std::uint32_t> plan_order;
    std::unordered_map<std::uint32_t, std::size_t> plan_pos;
    plan_order.reserve(ordered.size());
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (dirty[i]) {
            plan_pos[ordered[i]] = plan_order.size();
            plan_order.push_back(ordered[i]);
        }
    }

    Plan plan;
    plan.order = plan_order;
    plan.pending.assign(plan_order.size(), 0);
    plan.dependents.assign(plan_order.size(), {});
    for (std::size_t i = 0; i < plan_order.size(); ++i) {
        const auto& edge = ir.edges[plan_order[i]];
        auto count_deps = [&](const std::vector<std::string>& paths) {
            for (const auto& p : paths) {
                auto it = by_output.find(p);
                if (it == by_output.end()) {
                    continue;
                }
                auto plan_it = plan_pos.find(it->second);
                if (plan_it == plan_pos.end()) {
                    continue;
                }
                ++plan.pending[i];
                plan.dependents[plan_it->second].push_back(i);
            }
        };
        count_deps(edge.inputs);
        count_deps(edge.implicit_deps);
        count_deps(edge.order_only_deps);
    }

    RunResult result;
    result.total_edges = plan.order.size();

    if (plan.order.empty()) {
        return result;
    }

    // Mutex around the live log (workers append entries; main thread reads it
    // through on_progress).
    std::mutex log_mtx;

    auto execute_edge = [&](std::uint32_t edge_idx, const std::atomic<bool>& /*cancel*/) -> EdgeOutcome {
        const auto& edge = ir.edges[edge_idx];
        EdgeOutcome out;
        if (edge.command.empty()) {
            // Phony edges: nothing to run. Build log entry is still recorded so
            // downstream dirty checks observe propagation.
            out.exit_code = 0;
            return out;
        }
        bool inherit = edge.pool < ir.pools.size() && ir.pools[edge.pool].depth == 1;
        std::string output;
        auto rc = Process::run(edge.command, output, inherit);
        if (!rc) {
            out.err_message = rc.error().message;
            out.exit_code = -1;
            out.captured_output = std::move(output);
            return out;
        }
        out.exit_code = *rc;
        out.captured_output = std::move(output);
        return out;
    };

    Verbosity verbosity = Verbosity::Default;
    if (opts.very_verbose) {
        verbosity = Verbosity::FullCommand;
    } else if (opts.verbose) {
        verbosity = Verbosity::NonTty;
    }
    Progress progress(plan.order.size(), verbosity);

    auto on_progress = [&](std::size_t done, std::size_t total, std::uint32_t edge_idx, const EdgeOutcome& outcome) {
        (void)total;
        const auto& edge = ir.edges[edge_idx];
        progress.on_edge(done, edge_idx, edge, outcome);

        if (outcome.skipped) {
            return;
        }
        if (outcome.exit_code != 0 || !outcome.err_message.empty()) {
            return;
        }

        // Update log entry for this edge: re-hash outputs and parse depfile.
        std::lock_guard lk(log_mtx);
        auto pos = plan_pos.find(edge_idx);
        if (pos == plan_pos.end()) {
            return;
        }
        // Find ordered position to grab the precomputed pre-run entry.
        auto ord_it = pos_in_ordered.find(edge_idx);
        if (ord_it == pos_in_ordered.end()) {
            return;
        }
        LogEntry entry = std::move(checks[ord_it->second].entry);
        entry.last_run_ns = now_ns();
        // Re-stat the inputs: dependencies that ran in this build will have
        // rewritten the underlying files, so the pre-run hashes captured by
        // compute_dirty are stale. Use the pre-run TrackedFiles as the fast-path
        // fallback (mtime changed → re-hash).
        std::vector<std::string> input_paths;
        input_paths.reserve(edge.inputs.size() + edge.implicit_deps.size());
        for (const auto& p : edge.inputs) {
            input_paths.push_back(p);
        }
        for (const auto& p : edge.implicit_deps) {
            input_paths.push_back(p);
        }
        std::vector<TrackedFile> refreshed_inputs;
        bool _ignored = false;
        if (auto r = refresh_list(input_paths, entry.inputs, refreshed_inputs, _ignored); r) {
            entry.inputs = std::move(refreshed_inputs);
        }
        // Re-stat + re-hash outputs after the edge produced them.
        std::vector<TrackedFile> refreshed_outputs;
        if (auto r = refresh_list(edge.outputs, entry.outputs, refreshed_outputs, _ignored); r) {
            entry.outputs = std::move(refreshed_outputs);
        }
        // Parse depfile (if any) and refresh discovered headers from the fresh
        // .d file rather than carrying forward the prior list verbatim.
        if (!edge.depfile.empty()) {
            if (auto deps = parse_depfile(edge.depfile); deps) {
                bool _hm = false;
                std::vector<TrackedFile> refreshed_headers;
                if (auto r = refresh_list(*deps, entry.discovered_headers, refreshed_headers, _hm); r) {
                    entry.discovered_headers = std::move(refreshed_headers);
                }
            }
        }
        log.upsert(edge.name, std::move(entry));
    };

    ScheduleOptions sopts;
    sopts.jobs = opts.jobs;
    sopts.keep_going = opts.keep_going;
    Scheduler scheduler(ir, std::move(plan), sopts);
    auto sched_result = scheduler.run(execute_edge, on_progress);
    result.executed = sched_result.executed;
    result.skipped = sched_result.skipped;
    result.failures = sched_result.failures;
    result.interrupted = sched_result.interrupted;
    progress.on_finish(result.executed, result.failures, result.skipped, result.interrupted);

    if (auto r = log.save(log_path); !r) {
        std::cerr << "warning: failed to save build log: " << r.error().message << "\n";
    }

    return result;
}

} // namespace ngen::run
