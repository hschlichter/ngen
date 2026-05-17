#pragma once

#include "../ir/schema.hpp"
#include "scheduler.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unistd.h>

namespace ngen::run {

enum class Verbosity {
    Default,    // tty: single-line \r overwrite; non-tty: one line per edge
    NonTty,     // force one line per edge regardless of tty
    FullCommand // echo each command before running; one line per edge
};

class Progress {
public:
    Progress(std::size_t total, Verbosity v) : total_(total), verbosity_(v) {
        is_tty_ = ::isatty(STDOUT_FILENO) != 0;
        const char* no_color = std::getenv("NO_COLOR");
        color_ = is_tty_ && (no_color == nullptr || no_color[0] == '\0');
        // \r-overwrite only makes sense for default-mode + tty. Otherwise stream
        // one line per edge.
        overwrite_ = is_tty_ && verbosity_ == Verbosity::Default;
    }

    auto on_edge(std::size_t done, std::uint32_t /*edge_idx*/, const build::ir::Edge& edge, const EdgeOutcome& outcome) -> void {
        std::lock_guard lk(mtx_);
        if (outcome.skipped) {
            return;
        }
        bool failed = outcome.exit_code != 0 || !outcome.err_message.empty();
        const auto& desc = edge.description.empty() ? edge.name : edge.description;

        if (failed) {
            clear_overwrite_line();
            std::fprintf(stderr, "%s[%zu/%zu] FAILED:%s %s\n", red(), done, total_, reset(), desc.c_str());
            if (!outcome.captured_output.empty()) {
                std::fwrite(outcome.captured_output.data(), 1, outcome.captured_output.size(), stderr);
                if (outcome.captured_output.back() != '\n') {
                    std::fputc('\n', stderr);
                }
            }
            std::fprintf(stderr, "%s$%s %s\n", dim(), reset(), edge.command.c_str());
            if (!outcome.err_message.empty()) {
                std::fprintf(stderr, "%s%s%s\n", red(), outcome.err_message.c_str(), reset());
            }
            std::fflush(stderr);
            last_overwrite_ = 0;
            return;
        }

        bool has_output = !outcome.captured_output.empty();
        bool show_command = verbosity_ == Verbosity::FullCommand;
        if (overwrite_ && !has_output && !show_command) {
            // Same-line overwrite: print, \r, no newline. Use \e[K to clear remainder.
            std::string line = format_line(done, desc);
            std::fputc('\r', stdout);
            std::fputs(line.c_str(), stdout);
            std::fputs("\x1b[K", stdout);
            std::fflush(stdout);
            last_overwrite_ = line.size();
        } else {
            clear_overwrite_line();
            std::fputs(format_line(done, desc).c_str(), stdout);
            std::fputc('\n', stdout);
            if (show_command) {
                std::fprintf(stdout, "%s$%s %s\n", dim(), reset(), edge.command.c_str());
            }
            if (has_output) {
                std::fwrite(outcome.captured_output.data(), 1, outcome.captured_output.size(), stdout);
                if (outcome.captured_output.back() != '\n') {
                    std::fputc('\n', stdout);
                }
            }
            std::fflush(stdout);
        }
    }

    auto on_finish(std::size_t executed, std::size_t failures, std::size_t skipped, bool interrupted) -> void {
        std::lock_guard lk(mtx_);
        clear_overwrite_line();
        if (interrupted) {
            std::fprintf(stderr, "%sinterrupted%s after %zu edges (%zu failed)\n", red(), reset(), executed, failures);
            return;
        }
        if (failures > 0) {
            std::string tail;
            if (skipped > 0) {
                tail = ", " + std::to_string(skipped) + " skipped";
            }
            std::fprintf(stderr, "%s%zu edge(s) failed%s; %zu succeeded%s\n", red(), failures, reset(), executed, tail.c_str());
        }
        // Don't trumpet success — the last per-edge line is enough.
    }

private:
    auto clear_overwrite_line() -> void {
        if (last_overwrite_ > 0) {
            std::fputs("\r\x1b[K", stdout);
            std::fflush(stdout);
            last_overwrite_ = 0;
        }
    }

    auto format_line(std::size_t done, const std::string& desc) const -> std::string {
        char prefix[64];
        std::snprintf(prefix, sizeof(prefix), "[%zu/%zu]", done, total_);
        std::string out;
        if (color_) {
            out += "\x1b[36m";
            out += prefix;
            out += "\x1b[0m ";
        } else {
            out += prefix;
            out += ' ';
        }
        out += desc;
        return out;
    }

    auto red() const -> const char* { return color_ ? "\x1b[31m" : ""; }
    auto reset() const -> const char* { return color_ ? "\x1b[0m" : ""; }
    auto dim() const -> const char* { return color_ ? "\x1b[2m" : ""; }

    std::size_t total_;
    Verbosity verbosity_;
    bool is_tty_ = false;
    bool color_ = false;
    bool overwrite_ = false;
    std::size_t last_overwrite_ = 0;
    std::mutex mtx_;
};

} // namespace ngen::run
