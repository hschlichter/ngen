#include "rhidevicevulkan.h"
#include "rhitestenv.h"
#include "testharness.h"

#include <cstdint>
#include <filesystem>
#include <print>

// Suite state: one surfaceless, validation-enabled device for the whole run.
// Tests reach it through rhiTestDevice() and never name the backend type —
// this file is the single construction site.
static RhiDevice* g_device = nullptr;
static std::string g_shaderDir;
static uint64_t g_errorsBefore = 0;
static uint64_t g_warningsBefore = 0;

auto rhiTestDevice() -> RhiDevice* {
    return g_device;
}

auto rhiTestShaderPath(const char* filename) -> std::string {
    return (std::filesystem::path(g_shaderDir) / filename).string();
}

static auto snapshotValidationCounters(ngentest::TestContext& ctx) -> void {
    (void) ctx;
    g_errorsBefore = g_device->validationErrorCount();
    g_warningsBefore = g_device->validationWarningCount();
}

static auto checkValidationCounters(ngentest::TestContext& ctx) -> void {
    auto errorDelta = g_device->validationErrorCount() - g_errorsBefore;
    auto warningDelta = g_device->validationWarningCount() - g_warningsBefore;
    ctx.expect(errorDelta == 0, "validation errors were emitted during the test");
    ctx.expect(warningDelta == 0, "validation warnings were emitted during the test");
}

auto main(int argc, char* argv[]) -> int {
    // Shaders are built next to the executable (_out/<plat>/<cfg>/shaders/), so
    // resolve them relative to argv[0] rather than the working directory.
    g_shaderDir = (std::filesystem::path(argv[0]).parent_path() / "shaders").string();

    RhiDeviceVulkan device;
    if (!device.init({.window = nullptr, .enableValidation = true})) {
        std::println(stderr, "Failed to initialize surfaceless Vulkan device");
        return 1;
    }
    g_device = &device;

    auto failedCount = ngentest::runTests(
        argc,
        argv,
        {
            .beforeEach = &snapshotValidationCounters,
            .afterEach = &checkValidationCounters,
        });

    device.waitIdle();
    g_device = nullptr;
    device.destroy();

    return failedCount;
}
