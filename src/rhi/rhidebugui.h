#pragma once

#include "rhitypes.h"

#include <functional>

class RhiDevice;
class RhiCommandBuffer;
union SDL_Event;
struct SDL_Window;

struct RhiDebugUIInitInfo {
    SDL_Window* window;
    RhiDevice* device;
    RhiFormat colorFormat;
    uint32_t imageCount;
};

class RhiDebugUI {
public:
    RhiDebugUI() = default;
    RhiDebugUI(const RhiDebugUI&) = delete;
    RhiDebugUI& operator=(const RhiDebugUI&) = delete;
    RhiDebugUI(RhiDebugUI&&) = default;
    RhiDebugUI& operator=(RhiDebugUI&&) = default;
    virtual ~RhiDebugUI() = default;

    virtual auto init(const RhiDebugUIInitInfo& info) -> void = 0;
    virtual auto processEvent(SDL_Event* event) -> bool = 0;
    virtual auto render(RhiCommandBuffer* cmd) -> void = 0;
    virtual auto shutdown() -> void = 0;

    void setDrawCallback(std::function<void()> cb) { drawCallback = std::move(cb); }

protected:
    std::function<void()> drawCallback;
};
