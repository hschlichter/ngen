#pragma once

#include "rhitypes.h"

#include <functional>

class RhiDevice;
class RhiCommandBuffer;
union SDL_Event;
struct SDL_Window;

struct RhiEditorUIInitInfo {
    SDL_Window* window;
    RhiDevice* device;
    RhiFormat colorFormat;
    uint32_t imageCount;
};

class RhiEditorUI {
public:
    RhiEditorUI() = default;
    RhiEditorUI(const RhiEditorUI&) = delete;
    RhiEditorUI& operator=(const RhiEditorUI&) = delete;
    RhiEditorUI(RhiEditorUI&&) = default;
    RhiEditorUI& operator=(RhiEditorUI&&) = default;
    virtual ~RhiEditorUI() = default;

    virtual auto init(const RhiEditorUIInitInfo& info) -> void = 0;
    virtual auto processEvent(SDL_Event* event) -> bool = 0;
    virtual auto render(RhiCommandBuffer* cmd) -> void = 0;
    virtual auto shutdown() -> void = 0;

    void setDrawCallback(std::function<void()> cb) { drawCallback = std::move(cb); }

protected:
    std::function<void()> drawCallback;
};
