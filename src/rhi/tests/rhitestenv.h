#pragma once

#include <string>

class RhiDevice;

// The suite's shared device — created surfaceless with validation enabled by
// main before any test runs. Tests speak only the abstract RHI through this.
auto rhiTestDevice() -> RhiDevice*;

// Absolute path to a built test shader (e.g. "testflat.vert.spv"). Resolved
// relative to the executable, which sits next to the build's shaders/ output
// directory, so tests find fresh SPIR-V regardless of the working directory.
auto rhiTestShaderPath(const char* filename) -> std::string;
