#pragma once

#include "../framework/glob.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <string>
#include <string_view>
#include <sys/wait.h>

namespace ngen::run {

struct Process {
    // Run a shell command synchronously. When `inherit_stdio` is true, the child
    // inherits the parent's stdio (used for console-pool tools that drive an
    // interactive terminal). Otherwise stdout and stderr are merged into
    // `out_combined` and returned to the caller for buffered display.
    static auto run(std::string_view command, std::string& out_combined, bool inherit_stdio = false) -> std::expected<int, build::Error> {
        out_combined.clear();
        if (inherit_stdio) {
            // The child inherits stdio; no capture. std::system invokes /bin/sh -c.
            int rc = std::system(std::string(command).c_str()); // NOLINT(bugprone-command-processor)
            if (rc == -1) {
                return std::unexpected(build::Error{"failed to spawn shell for: " + std::string(command)});
            }
            return WIFEXITED(rc) ? WEXITSTATUS(rc) : 128 + WTERMSIG(rc);
        }
        // popen + 2>&1: combine streams, read until EOF, then close to get the exit status.
        std::string merged = std::string(command) + " 2>&1";
        FILE* pipe = popen(merged.c_str(), "r");
        if (!pipe) {
            return std::unexpected(build::Error{"popen failed for: " + std::string(command)});
        }
        std::array<char, 4096> buf{};
        while (auto n = std::fread(buf.data(), 1, buf.size(), pipe)) {
            out_combined.append(buf.data(), n);
        }
        int status = pclose(pipe);
        if (status == -1) {
            return std::unexpected(build::Error{"pclose failed for: " + std::string(command)});
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
};

} // namespace ngen::run
