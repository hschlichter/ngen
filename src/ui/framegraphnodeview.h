#pragma once

#include "framegraphdebug.h"

#include <cstdint>
#include <optional>

void drawFrameGraphNodeView(const FrameGraphDebugSnapshot& snap, std::optional<uint32_t>& selPass, std::optional<uint32_t>& selResource);
