#pragma once

#include <string>
#include <vector>

// RenderDoc in-app API. Load before the graphics device is created so RenderDoc can hook it:
// uses the library when RenderDoc injected it (launched from qrenderdoc), or loads
// librenderdoc.so when forceLoad is set (--renderdoc). The library is resolved at runtime;
// nothing links against it. Compiled to a no-op without NGEN_INTROSPECTION.
class RenderDocCapture {
public:
    auto load(bool forceLoad) -> void;
    auto available() const -> bool { return api != nullptr; }
    // Captures the next presented frame.
    auto triggerCapture() -> bool;
    // Paths of captures completed since the last call.
    auto pollNewCaptures() -> std::vector<std::string>;
    // Opens a capture in qrenderdoc (the replay UI), connected to this process.
    auto openInUi(const std::string& path) -> bool;
    auto lastCapture() const -> const std::string& { return latest; }

private:
    void* api = nullptr; // RENDERDOC_API_1_6_0*
    unsigned seenCaptures = 0;
    std::string latest;
};
