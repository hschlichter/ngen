#pragma once

#include "camera.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

// Camera window: current pose in the --camera flag syntax, jump, the cameras the scene authors, and bookmarks kept in a
// text file next to the scene (one "name x y z yaw pitch" per line).
struct CameraBookmark {
    std::string name;
    float pose[5] = {};
};

// The debug camera: while on, the view renders from it, and the main camera (the player) keeps deciding residency, LOD and, unless
// culling is from the camera rendered from, culling. The window edits it directly; ngen-view applies it each frame (src/view/README.md,
// "The debug camera").
struct DebugCameraState {
    bool on = false;
    Camera camera{};
    bool controlMain = false;    // the keys and mouse move the main camera instead, while the view stays on the debug camera
    bool cullFromMain = false;   // culling and the shadow cascades from the main camera instead of the one rendered from
    bool trail = true;           // the main camera's trail drawn
    bool placeRequested = false; // put it behind and above the main camera
    // The main camera's trail: a point each time it moved trailSpacingMetres from the last, the last trailCount, kept while the debug
    // camera is on. By distance moved, never by time.
    std::vector<glm::vec3> trailPoints;
    static constexpr float trailSpacingMetres = 2.0f;
    static constexpr size_t trailCount = 500;
};

struct CameraWindowState {
    std::string bookmarkPath; // "" = no scene file, bookmarks not persisted
    std::vector<CameraBookmark> bookmarks;
    std::vector<CameraBookmark> sceneCameras; // the cameras the scene authors, named by prim path; set by the view
    bool loaded = false;
    char jumpText[128] = "";
    char newName[64] = "";
    bool requestFrameScene = false;
    bool requestFrameSelected = false;
    DebugCameraState debug;
};

void drawCameraWindow(bool& show, Camera& camera, CameraWindowState& state);
