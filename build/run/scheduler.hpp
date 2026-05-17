// ngen::run::Scheduler — dependency-aware `-j N` worker pool for IR edges.
//
// Given a `Plan` (topologically ordered dirty edges + per-edge pending count + dependents map), spawn N worker
// threads, hand them ready edges, and harvest results until everything has been processed. The Scheduler is
// deliberately a plain class with two callbacks — `ExecuteFn` to run an edge, `ProgressFn` to report
// completion — rather than an integrated runner, so the concurrency machinery stays separate from the
// edge-execution and log-writeback machinery in `execute.hpp`.
//
// Concurrency model:
//   - Each `Plan.order` slot has an `EdgeState{ pending, alive }`. `pending` decrements as deps complete;
//     when it hits zero, the edge is enqueued onto the ready queue. `alive` flips to false when any transitive
//     dep fails or the build is cancelled — its dependents still flow through the queue but with
//     `outcome.skipped = true`, so they poison their own dependents in turn without running anything.
//   - Workers wait on a condition variable for ready edges or terminal state. State changes broadcast via
//     `cv.notify_all()`.
//   - **Console pool** (depth 1): a single `std::mutex` is held while any console-pool edge runs, so other
//     console-pool edges block but default-pool edges keep flowing in parallel.
//   - **SIGINT** installs a process-wide flag handler; workers poll it between edges and bail. Running
//     children get drained naturally by `Process::wait` when they exit on their own.
//
// `keep_going` is the cap on tolerated failures (default 1 = stop after the first); once exceeded, the cancel
// token is set, no new work is scheduled, and in-flight workers run to completion before the scheduler
// returns.
//
// Result counts: executed (success), failures (non-zero exit / launch error), skipped (cancelled or
// dead-branch).

#pragma once

#include "../framework/glob.hpp"
#include "../ir/schema.hpp"
#include "process.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ngen::run {

struct EdgeOutcome {
    int exit_code = 0;
    std::string captured_output;
    std::string err_message; // non-empty if process failed to launch
    bool skipped = false;    // upstream failed, cancelled, or otherwise didn't run
};

struct ScheduleResult {
    std::size_t executed = 0;
    std::size_t failures = 0;
    std::size_t skipped = 0;
    bool interrupted = false;
};

struct ScheduleOptions {
    int jobs = 1;
    int keep_going = 1;
};

// Plan handed to the scheduler. `order` is the dirty set in topological order;
// `dependents[k]` lists positions in `order` that depend on `order[k]`;
// `pending[k]` is the initial count of incomplete prerequisites for `order[k]`.
struct Plan {
    std::vector<std::uint32_t> order; // edge indices into IR.edges
    std::vector<std::vector<std::size_t>> dependents;
    std::vector<std::uint32_t> pending;
};

namespace detail {

inline std::atomic<bool>& sigint_flag() {
    static std::atomic<bool> flag{false};
    return flag;
}

inline auto install_sigint_handler() -> struct ::sigaction {
    struct ::sigaction prev{};
    struct ::sigaction sa{};
    sa.sa_handler = [](int) {
        sigint_flag().store(true, std::memory_order_relaxed);
    };
    sa.sa_flags = 0;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, &prev);
    return prev;
}

inline auto
restore_sigint_handler(const struct ::sigaction& prev) -> void {
    ::sigaction(SIGINT, &prev, nullptr);
}

} // namespace detail

class Scheduler {
public:
    using ExecuteFn = std::function<EdgeOutcome(std::uint32_t edge_idx, const std::atomic<bool>& cancel_token)>;
    using ProgressFn = std::function<void(std::size_t done, std::size_t total, std::uint32_t edge_idx, const EdgeOutcome&)>;

    Scheduler(const build::ir::IR& ir, Plan plan, ScheduleOptions opts) : ir_(ir), plan_(std::move(plan)), opts_(opts) {}

    auto run(ExecuteFn execute_edge, ProgressFn on_progress) -> ScheduleResult {
        ScheduleResult result;
        if (plan_.order.empty()) {
            return result;
        }

        auto prev_handler = detail::install_sigint_handler();
        detail::sigint_flag().store(false, std::memory_order_relaxed);

        struct EdgeState {
            std::uint32_t pending = 0;
            bool alive = true; // false = a transitive upstream failed
        };
        std::vector<EdgeState> state(plan_.order.size());
        for (std::size_t i = 0; i < plan_.order.size(); ++i) {
            state[i].pending = plan_.pending[i];
        }

        std::mutex m;
        std::condition_variable cv;
        std::queue<std::size_t> ready_q;
        std::size_t in_flight = 0;
        std::size_t processed = 0;
        std::atomic<bool> cancel{false};

        for (std::size_t i = 0; i < plan_.order.size(); ++i) {
            if (state[i].pending == 0) {
                ready_q.push(i);
            }
        }

        // Console pool: depth 1. Held while a console edge runs.
        std::mutex console_mtx;

        auto propagate = [&](std::size_t plan_idx, bool succeeded) {
            for (auto dep_idx : plan_.dependents[plan_idx]) {
                if (!succeeded) {
                    state[dep_idx].alive = false;
                }
                if (state[dep_idx].pending > 0) {
                    --state[dep_idx].pending;
                    if (state[dep_idx].pending == 0) {
                        ready_q.push(dep_idx);
                    }
                }
            }
        };

        auto worker_run = [&]() {
            while (true) {
                std::size_t plan_idx;
                {
                    std::unique_lock lk(m);
                    cv.wait(lk, [&] { return !ready_q.empty() || (in_flight == 0 && processed == plan_.order.size()); });
                    if (ready_q.empty()) {
                        cv.notify_all();
                        return;
                    }
                    plan_idx = ready_q.front();
                    ready_q.pop();
                    ++in_flight;
                }

                auto edge_idx = plan_.order[plan_idx];
                const auto& edge = ir_.edges[edge_idx];
                bool is_console = edge.pool < ir_.pools.size() && ir_.pools[edge.pool].depth == 1;
                bool alive_at_start;
                {
                    std::lock_guard lk(m);
                    alive_at_start = state[plan_idx].alive;
                }

                EdgeOutcome outcome;
                if (!alive_at_start || cancel.load(std::memory_order_relaxed) || detail::sigint_flag().load(std::memory_order_relaxed)) {
                    outcome.skipped = true;
                } else {
                    std::unique_lock<std::mutex> console_lock(console_mtx, std::defer_lock);
                    if (is_console) {
                        console_lock.lock();
                    }
                    outcome = execute_edge(edge_idx, cancel);
                }

                bool failed = !outcome.skipped && (outcome.exit_code != 0 || !outcome.err_message.empty());

                {
                    std::lock_guard lk(m);
                    ++processed;
                    --in_flight;
                    if (outcome.skipped) {
                        ++result.skipped;
                    } else if (failed) {
                        ++result.failures;
                    } else {
                        ++result.executed;
                    }
                    on_progress(processed, plan_.order.size(), edge_idx, outcome);

                    if (failed && static_cast<int>(result.failures) >= opts_.keep_going) {
                        cancel.store(true, std::memory_order_relaxed);
                    }

                    propagate(plan_idx, !failed && !outcome.skipped);
                }
                cv.notify_all();

                if (detail::sigint_flag().load(std::memory_order_relaxed)) {
                    cancel.store(true, std::memory_order_relaxed);
                    cv.notify_all();
                }
            }
        };

        int j = std::max(1, opts_.jobs);
        std::vector<std::thread> workers;
        workers.reserve(j);
        for (int i = 0; i < j; ++i) {
            workers.emplace_back(worker_run);
        }
        for (auto& t : workers) {
            t.join();
        }

        if (detail::sigint_flag().load(std::memory_order_relaxed)) {
            result.interrupted = true;
        }
        detail::restore_sigint_handler(prev_handler);
        return result;
    }

private:
    const build::ir::IR& ir_;
    Plan plan_;
    ScheduleOptions opts_;
};

} // namespace ngen::run
