#pragma once

#include "../framework/glob.hpp"
#include "../framework/ir/schema.hpp"
#include "process.hpp"

#include <cstdint>
#include <cstdio>
#include <expected>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ngen::run {

struct RunOptions {
    int jobs = 1;       // phase 2 ignores; phase 3 wires up the scheduler.
    int keep_going = 1; // stop after this many failures (default 1 = fail fast).
    bool verbose = false;
    bool very_verbose = false;
    std::vector<std::string> targets; // empty = use IR's default_targets.
};

struct RunResult {
    std::size_t total_edges = 0;
    std::size_t executed = 0;
    std::size_t failures = 0;
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

inline auto edge_is_console(const build::ir::IR& ir, const build::ir::Edge& edge) -> bool {
    return edge.pool < ir.pools.size() && ir.pools[edge.pool].depth == 1;
}

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

    // IR edges are emitted in post-order by the graph stage, so iterating the
    // edges array in index order yields a valid topological order for the
    // reachable subset.
    std::vector<std::uint32_t> plan;
    plan.reserve(reachable.size());
    for (std::uint32_t i = 0; i < ir.edges.size(); ++i) {
        if (reachable.contains(i)) {
            plan.push_back(i);
        }
    }

    RunResult result;
    result.total_edges = plan.size();

    std::size_t step = 0;
    for (auto idx : plan) {
        ++step;
        const auto& edge = ir.edges[idx];
        if (edge.command.empty()) {
            ++result.executed;
            continue;
        }

        const auto& desc = edge.description.empty() ? edge.name : edge.description;
        std::cout << "[" << step << "/" << plan.size() << "] " << desc << "\n";
        if (opts.very_verbose) {
            std::cout << "$ " << edge.command << "\n";
        }
        std::cout.flush();

        bool inherit = edge_is_console(ir, edge);
        std::string output;
        auto rc = Process::run(edge.command, output, inherit);
        if (!rc) {
            return std::unexpected(rc.error());
        }
        if (!output.empty()) {
            std::cout << output;
            if (output.back() != '\n') {
                std::cout << '\n';
            }
        }
        if (*rc != 0) {
            ++result.failures;
            std::cerr << "FAILED: ";
            for (std::size_t i = 0; i < edge.outputs.size(); ++i) {
                std::cerr << (i ? " " : "") << edge.outputs[i];
            }
            std::cerr << "\n$ " << edge.command << "\n";
            if (static_cast<int>(result.failures) >= opts.keep_going) {
                return result;
            }
            continue;
        }
        ++result.executed;
    }
    return result;
}

} // namespace ngen::run
