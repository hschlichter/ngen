#include "testharness.h"

#include "jsonlinesfilesink.h"
#include "observationbus.h"
#include "observationmacros.h"

#include <memory>
#include <print>
#include <utility>

namespace ngentest {

namespace {

struct RegisteredTest {
    const char* name;
    TestFunc func;
};

auto registry() -> std::vector<RegisteredTest>& {
    static std::vector<RegisteredTest> tests;
    return tests;
}

} // namespace

auto registerTest(const char* name, TestFunc func) -> bool {
    registry().push_back({.name = name, .func = func});
    return true;
}

auto runTests(int argc, char** argv, const TestRunOptions& options) -> int {
    std::string obsOutputPath;
    std::string testFilter;
    std::string dumpDir;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg.starts_with("--obs-output=")) {
            obsOutputPath = std::string(arg.substr(std::string_view("--obs-output=").size()));
        } else if (arg.starts_with("--test=")) {
            testFilter = std::string(arg.substr(std::string_view("--test=").size()));
        } else if (arg.starts_with("--dump-dir=")) {
            dumpDir = std::string(arg.substr(std::string_view("--dump-dir=").size()));
        } else {
            std::println(stderr, "Unknown argument: {}", arg);
            return 1;
        }
    }

    if (!obsOutputPath.empty()) {
        auto sink = std::make_unique<obs::JsonLinesFileSink>();
        if (!sink->open(obsOutputPath)) {
            std::println(stderr, "Failed to open observation output: {}", obsOutputPath);
            return 1;
        }
        obs::bus().setSink(std::move(sink));
    }

    int ranCount = 0;
    int failedCount = 0;
    for (const auto& test : registry()) {
        if (!testFilter.empty() && testFilter != test.name) {
            continue;
        }
        ranCount++;

        OBS_EVENT("Test", "TestStarted", test.name);
        TestContext ctx(test.name, dumpDir);
        if (options.beforeEach != nullptr) {
            options.beforeEach(ctx);
        }
        test.func(ctx);
        if (options.afterEach != nullptr) {
            options.afterEach(ctx);
        }

        if (ctx.failed()) {
            failedCount++;
            for (const auto& message : ctx.failures()) {
                std::println(stderr, "[FAIL] {}: {}", test.name, message);
                OBS_EVENT("Test", "TestFailed", test.name).field("message", message);
            }
        } else {
            std::println("[PASS] {}", test.name);
            OBS_EVENT("Test", "TestPassed", test.name);
        }
    }

    if (ranCount == 0) {
        std::println(stderr, "No tests matched{}{}", testFilter.empty() ? "" : ": ", testFilter);
        failedCount = 1;
    }

    std::println("{} of {} tests passed", ranCount - failedCount, ranCount);

    // Drain and flush the observation stream before exit. No-op without a sink.
    obs::bus().shutdown();

    return failedCount;
}

} // namespace ngentest
