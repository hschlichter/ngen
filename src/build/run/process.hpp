// ngen::run::Process — subprocess primitives for the runner.
//
// Three operations: `spawn` (start the command with its stdout and stderr merged onto a pipe), `wait` (drain the
// pipe and reap), and `signal` (forward a signal to the child). Plus a `run` convenience wrapper that spawns and
// waits in one call — used by `execute.hpp`'s edge-execution callback.
//
// `Handle` carries the live child and the read end of the stdout/stderr pipe. `inherit_stdio = true` short-circuits
// pipe creation and lets the child write directly to the parent's terminal — used for console-pool edges
// (`clean`, `format`, `tidy`) where live, interleaved output is wanted.
//
// All errors return `std::expected<int, build::Error>`. The exit code is the normal-termination value, or
// `128 + signal_number` for signalled exits on Linux.
//
// Two implementations behind one interface; every subprocess call in the build system goes through this header:
//   - Linux: fork + `pipe(2)` + `execv` of `/bin/sh -c <command>`, `waitpid` to reap.
//   - Windows: `CreateProcessW` with the command line as is (no shell; `shell_quote` quotes with the Windows
//     command-line rules), an inheritable anonymous pipe, `WaitForSingleObject` + `GetExitCodeProcess` to reap.

#pragma once

#include "../framework/glob.hpp"

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include "win32.hpp"
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ngen::run {

#if defined(_WIN32)

struct Process {
    struct Handle {
        HANDLE process = nullptr;
        HANDLE stdout_read = nullptr; // read end of the pipe; null when child inherits stdio.
        bool inherited = false;
    };

    // Spawn a child running `command`. When `inherit_stdio` is true the child inherits the parent's stdio (used for
    // the console pool); otherwise stdout and stderr are merged onto a pipe whose read end lives on the handle.
    static auto spawn(std::string_view command, bool inherit_stdio) -> std::expected<Handle, build::Error> {
        Handle h;
        h.inherited = inherit_stdio;

        HANDLE write_end = nullptr;
        if (!inherit_stdio) {
            SECURITY_ATTRIBUTES security{};
            security.nLength = sizeof(security);
            security.bInheritHandle = TRUE;
            if (!::CreatePipe(&h.stdout_read, &write_end, &security, 0)) {
                return std::unexpected(build::Error{"CreatePipe failed: " + win32::last_error_text()});
            }
            // Only the write end goes to the child.
            ::SetHandleInformation(h.stdout_read, HANDLE_FLAG_INHERIT, 0);
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (!inherit_stdio) {
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = write_end;
            startup.hStdError = write_end;
        }

        // CreateProcessW may write into the command line buffer, so it gets its own copy.
        auto command_line = win32::widen(command);
        PROCESS_INFORMATION info{};
        BOOL created = ::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &info);
        if (write_end) {
            ::CloseHandle(write_end);
        }
        if (!created) {
            auto message = "CreateProcess failed for `" + std::string(command) + "`: " + win32::last_error_text();
            if (h.stdout_read) {
                ::CloseHandle(h.stdout_read);
                h.stdout_read = nullptr;
            }
            return std::unexpected(build::Error{message});
        }
        ::CloseHandle(info.hThread);
        h.process = info.hProcess;
        return h;
    }

    // Drain the child's output (if captured) and reap. Returns the child's exit code. Closes the pipe and the
    // process handle; the handle is consumed.
    static auto wait(Handle& h, std::string& out_combined) -> std::expected<int, build::Error> {
        out_combined.clear();
        if (!h.inherited && h.stdout_read) {
            std::array<char, 4096> buf{};
            while (true) {
                DWORD n = 0;
                if (!::ReadFile(h.stdout_read, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) || n == 0) {
                    // ERROR_BROKEN_PIPE: every writer closed, the child is done writing.
                    break;
                }
                out_combined.append(buf.data(), n);
            }
            ::CloseHandle(h.stdout_read);
            h.stdout_read = nullptr;
        }

        ::WaitForSingleObject(h.process, INFINITE);
        DWORD code = 1;
        BOOL got = ::GetExitCodeProcess(h.process, &code);
        ::CloseHandle(h.process);
        h.process = nullptr;
        if (!got) {
            return std::unexpected(build::Error{"GetExitCodeProcess failed: " + win32::last_error_text()});
        }
        return static_cast<int>(code);
    }

    // Ctrl+C already reaches every process on the console; any other signal ends the child.
    static auto signal(const Handle& h, int sig) -> void {
        if (h.process && sig != SIGINT) {
            ::TerminateProcess(h.process, 128 + static_cast<UINT>(sig));
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

#else

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

#endif

} // namespace ngen::run
