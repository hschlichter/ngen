// ngen-asset-server: packs assets on request and streams them to the tools that asked (src/asset/README.md).
//
// Started by hand; runs until SIGINT or SIGTERM. Its working directory is the root: asset ids are paths relative to it,
// packers run in it, the cache is .ngen-assets/ in it, and clients in the same directory find it through
// .ngen-discovery/. The variant comes from the binary's directory, _out/<platform>/<config>/, and the pack rules from
// the project's pack.cpp, compiled in.

#include "assetserver.h"
#include "assettrace.h"

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
    if (ec) {
        std::println(stderr, "ngen-asset-server: cannot find its own executable: {}", ec.message());
        return 1;
    }
    AssetServer::Options options;
    options.binDirectory = bin;
    options.config = bin.filename().string();
    options.platform = bin.parent_path().filename().string();

    AssetServer server;
    if (auto started = server.start(options); !started) {
        std::println(stderr, "ngen-asset-server: {}", started.error());
        return 1;
    }
    trace("ngen-asset-server {} in {} on 127.0.0.1:{}, pid {}", server.variant(), fs::current_path().string(), server.port(), getpid());

    int received = 0;
    sigwait(&signals, &received);
    trace("{}: stopping", received == SIGINT ? "SIGINT" : "SIGTERM");
    server.stop();
    trace("stopped");
    return 0;
}
