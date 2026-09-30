// ngen-asset-server: packs assets on request and streams them to the tools that asked (src/asset/README.md).
//
// Started by hand from its variant's directory, _out/<platform>/<config>/ngen-asset-server; runs until SIGINT or
// SIGTERM. The variant comes from where the binary lives, and the pack rules from the project's pack.cpp,
// compiled in.

#include "assetserver.h"
#include "assettrace.h"
#include "rpcdiscovery.h"

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <print>
#include <pthread.h>
#include <unistd.h>

namespace fs = std::filesystem;

auto main(int argc, char** argv) -> int {
    if (argc > 1) {
        std::println(stderr, "usage: {}   (no arguments; stop it with Ctrl-C)", argv[0]);
        return 2;
    }
    // Signals are waited for on the main thread; every other thread starts with them blocked.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    std::error_code ec;
    auto exe = fs::canonical("/proc/self/exe", ec);
    auto bin = exe.parent_path();
    if (ec || bin.parent_path().parent_path().filename() != "_out") {
        std::println(stderr, "ngen-asset-server: run it from its variant directory, _out/<platform>/<config>/");
        return 1;
    }
    AssetServer::Options options;
    options.projectRoot = rpcProjectRoot();
    options.binDirectory = bin;
    options.config = bin.filename().string();
    options.platform = bin.parent_path().filename().string();
    // Asset ids are project-relative paths, and packers run with the project root as their working directory.
    fs::current_path(options.projectRoot, ec);
    if (ec) {
        std::println(stderr, "ngen-asset-server: cannot enter {}: {}", options.projectRoot.string(), ec.message());
        return 1;
    }

    AssetServer server;
    if (auto started = server.start(options); !started) {
        std::println(stderr, "ngen-asset-server: {}", started.error());
        return 1;
    }
    trace("ngen-asset-server {} on 127.0.0.1:{}, pid {}", server.variant(), server.port(), getpid());

    int received = 0;
    sigwait(&signals, &received);
    trace("{}: stopping", received == SIGINT ? "SIGINT" : "SIGTERM");
    server.stop();
    trace("stopped");
    return 0;
}
