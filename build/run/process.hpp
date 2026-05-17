// ngen::run::Process — POSIX subprocess primitives for the runner.
//
// Three operations: `spawn` (fork + `pipe(2)` for stdout capture + `execv` of `/bin/sh -c <command>`), `wait`
// (drain the pipe + `waitpid` to reap), and `signal` (forward a signal to the pid). Plus a `run` convenience
// wrapper that spawns and waits in one call — used by `execute.hpp`'s edge-execution callback.
//
// `Handle` carries the live pid and the read end of the stdout/stderr pipe. `inherit_stdio = true` short-circuits
// pipe creation and lets the child write directly to the parent's terminal — used for console-pool edges
// (`clean`, `format`, `tidy`) where live, interleaved output is wanted.
//
// All errors return `std::expected<int, build::Error>`. The exit code is the normal-termination value, or
// `128 + signal_number` for signalled exits.
//
// Linux-only. A Windows port replaces fork+exec with `CreateProcess` and the pipe with an `OVERLAPPED` read
// without touching anything else in the runner — every subprocess call in the codebase goes through this header.

#pragma once

#include "../framework/glob.hpp"

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace ngen::run {

struct Process {
    struct Handle {
        ::pid_t pid = -1;
        int stdout_fd = -1; // read end of the pipe; -1 when child inherits stdio.
        bool inherited = false;
    };

    // Spawn a child running `/bin/sh -c <command>`. When `inherit_stdio` is true
    // the child inherits the parent's stdio (used for the console pool); otherwise
    // stdout and stderr are merged onto a pipe whose read end lives on the handle.
    static auto spawn(std::string_view command, bool inherit_stdio) -> std::expected<Handle, build::Error> {
        Handle h;
        h.inherited = inherit_stdio;

        int pipefd[2] = {-1, -1};
        if (!inherit_stdio) {
            if (::pipe(pipefd) != 0) {
                return std::unexpected(build::Error{std::string("pipe failed: ") + std::strerror(errno)});
            }
        }

        ::pid_t pid = ::fork();
        if (pid < 0) {
            if (!inherit_stdio) {
                ::close(pipefd[0]);
                ::close(pipefd[1]);
            }
            return std::unexpected(build::Error{std::string("fork failed: ") + std::strerror(errno)});
        }

        if (pid == 0) {
            if (!inherit_stdio) {
                ::close(pipefd[0]);
                ::dup2(pipefd[1], STDOUT_FILENO);
                ::dup2(pipefd[1], STDERR_FILENO);
                if (pipefd[1] != STDOUT_FILENO && pipefd[1] != STDERR_FILENO) {
                    ::close(pipefd[1]);
                }
            }
            std::string cmd(command);
            const char* argv[] = {"/bin/sh", "-c", cmd.c_str(), nullptr};
            ::execv("/bin/sh", const_cast<char**>(argv));
            ::_exit(127);
        }

        if (!inherit_stdio) {
            ::close(pipefd[1]);
            h.stdout_fd = pipefd[0];
        }
        h.pid = pid;
        return h;
    }

    // Drain the child's output (if captured) and reap. Returns the exit code:
    // normal-exit status, or 128 + signal for abnormal termination. Closes
    // stdout_fd; the handle is consumed.
    static auto wait(Handle& h, std::string& out_combined) -> std::expected<int, build::Error> {
        out_combined.clear();
        if (!h.inherited && h.stdout_fd >= 0) {
            std::array<char, 4096> buf{};
            while (true) {
                auto n = ::read(h.stdout_fd, buf.data(), buf.size());
                if (n > 0) {
                    out_combined.append(buf.data(), static_cast<std::size_t>(n));
                    continue;
                }
                if (n == 0) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                ::close(h.stdout_fd);
                h.stdout_fd = -1;
                return std::unexpected(build::Error{std::string("read failed: ") + std::strerror(errno)});
            }
            ::close(h.stdout_fd);
            h.stdout_fd = -1;
        }

        int status = 0;
        while (true) {
            auto r = ::waitpid(h.pid, &status, 0);
            if (r == h.pid) {
                break;
            }
            if (r < 0 && errno == EINTR) {
                continue;
            }
            return std::unexpected(build::Error{std::string("waitpid failed: ") + std::strerror(errno)});
        }
        h.pid = -1;
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
            return 128 + WTERMSIG(status);
        }
        return 1;
    }

    static auto signal(const Handle& h, int sig) -> void {
        if (h.pid > 0) {
            ::kill(h.pid, sig);
        }
    }

    // Run a command and block for completion. Captures combined stdout+stderr
    // unless `inherit_stdio` is true. Convenience wrapper over spawn+wait.
    static auto run(std::string_view command, std::string& out_combined, bool inherit_stdio = false) -> std::expected<int, build::Error> {
        auto handle = spawn(command, inherit_stdio);
        if (!handle) {
            return std::unexpected(handle.error());
        }
        return wait(*handle, out_combined);
    }
};

} // namespace ngen::run
