#pragma once

#include "framegraphdebug.h"

#include <cstdint>
#include <optional>

void drawFrameGraphWindow(bool& show,
                          const std::optional<FrameGraphDebugSnapshot>& snap,
                          std::optional<uint32_t>& selPass,
                          std::optional<uint32_t>& selResource);
