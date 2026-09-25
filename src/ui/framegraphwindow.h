#pragma once

#include "framegraphdebug.h"

#include <cstdint>
#include <optional>
#include <string>

// A resource the user asked to capture from the Frame Graph window: after `pass` ("" = after
// the last pass).
struct FrameGraphCaptureRequest {
    std::string pass;
    std::string resource;
};

void drawFrameGraphWindow(bool& show,
                          const std::optional<FrameGraphDebugSnapshot>& snap,
                          std::optional<uint32_t>& selPass,
                          std::optional<uint32_t>& selResource,
                          std::optional<FrameGraphCaptureRequest>& captureRequest);
