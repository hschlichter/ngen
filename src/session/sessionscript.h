#pragma once

// Session commands: the verbs an agent or a script uses to drive ngen-view without the
// UI. CLI flags and the --script file both produce the same commands; main applies the
// ones due at the top of each frame.
//
//   camera x,y,z,yaw,pitch        set the camera pose (degrees)
//   camera-frame scene|/prim/path frame the whole scene or one prim
//   select /prim/path             select a prim
//   translate /prim/path dx,dy,dz move a prim by an offset in its local space (preview edit, no layer write)
//   view lit|albedo|normals|depth|shadowfactor|shadowmap|shadowuv|worldpos
//   overlay name=on|off[,...]     grid, origin, gizmo, aabbs, lightgizmos, buffer, shadow, aa
//   screenshot PATH               write the presented frame as PNG
//   dump-render-debug PATH        write the render debug snapshot as JSON
//   dump-profile PATH             write the profiler history as Chrome trace JSON
//   quit
//
// Script lines: "<frame> <verb> [args]"; '#' starts a comment.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct SessionCommand {
    uint64_t frame = 0;
    std::string verb;
    std::string args;
};

class SessionScript {
public:
    // Parses one line; returns false with `error` set on a malformed line.
    auto addLine(std::string_view line, std::string& error) -> bool;
    auto add(uint64_t frame, std::string verb, std::string args) -> void;
    auto loadFile(const char* path, std::string& error) -> bool;

    // Commands due at `frame`, in insertion order, removed from the queue.
    auto takeDue(uint64_t frame, std::vector<SessionCommand>& out) -> void;
    auto empty() const -> bool { return pending.empty(); }
    auto lastFrame() const -> uint64_t;

private:
    std::vector<SessionCommand> pending;
};

// Parses "x,y,z,yaw,pitch" into five floats; false on error.
auto parseCameraPose(std::string_view text, float out[5]) -> bool;
// Formats a pose in the same syntax.
auto formatCameraPose(const float pose[5]) -> std::string;
