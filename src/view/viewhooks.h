#pragma once

class ViewFrame;
struct RenderSnapshot;
union SDL_Event;

// Where the layers above the engine join ngen-view's frame (src/view/README.md, "The frame"): the main loop calls these at fixed points
// between the engine's steps. The tools (src/tools/toolshooks.h) and the UI (src/ui/uihooks.h) implement them; without them the frame is
// the engine's alone. Each does nothing unless overridden.
class ViewHooks {
public:
    virtual ~ViewHooks() = default;

    // After the scene update, before the commands: the lists built from a new scene snapshot.
    virtual auto afterScene(ViewFrame& /*frame*/) -> void {}
    // After the commands: the cameras a tool takes over (ViewFrame::cameras).
    virtual auto cameras(ViewFrame& /*frame*/) -> void {}
    // An input event from the window layer, before the view's controls; true when taken.
    virtual auto event(ViewFrame& /*frame*/, const SDL_Event& /*event*/) -> bool { return false; }
    // After the camera moved, culling read back and the cascades were fitted.
    virtual auto afterCamera(ViewFrame& /*frame*/) -> void {}
    // The frame's debug lines (ViewFrame::debugDraw), before the engine's own.
    virtual auto debugDraw(ViewFrame& /*frame*/) -> void {}
    // What the renderer should read back this frame (ViewFrame::readbacks), and then what came back.
    virtual auto wantReadbacks(ViewFrame& /*frame*/) -> void {}
    virtual auto takeReadbacks(ViewFrame& /*frame*/) -> void {}
    // The UI: its windows drawn, and what they asked for carried out.
    virtual auto drawUi(ViewFrame& /*frame*/) -> void {}
    // The tools' and the UI's parts of the snapshot: the gizmos, the tools' viewport layers, the UI's draw data.
    virtual auto fillSnapshot(ViewFrame& /*frame*/, RenderSnapshot& /*snapshot*/) -> void {}
};
