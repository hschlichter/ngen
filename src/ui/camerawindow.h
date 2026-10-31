#pragma once

#include "camera.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

// Camera window: current pose in the --camera flag syntax, jump, the cameras the scene authors, and bookmarks kept in a
// text file next to the scene (one "name x y z yaw pitch" per line).
struct CameraBookmark {
    std::string name;
    float pose[5] = {};
};

// The debug camera's state: its pose, and the main camera's trail. Its switches (on, what it drives, the controls, the trail drawn) are
// configuration values (`tools.debugcamera.*`, src/view/debugcamera.h); ngen-view applies them each frame (src/view/README.md, "The
// debug camera").
struct DebugCameraState {
    Camera camera{};
    bool placeRequested = false; // put it behind and above the main camera
    // The main camera's trail: a point each time it moved trailSpacingMetres from the last, the last trailCount, kept while the debug
    // camera is on. By distance moved, never by time.
    std::vector<glm::vec3> trailPoints;
    static constexpr float trailSpacingMetres = 2.0f;
    static constexpr size_t trailCount = 500;
};

// The camera modes' state, for the camera that has the controls: following the ground (its eye kept a height above the render geometry
// under it; on and the height are configuration values, `view.camera.follow*`) and orbiting (Left Alt held: right-drag turns around the
// surface under the cursor, the wheel moves toward it). ngen-view fills it in each frame (src/view/README.md, "Camera modes").
struct CameraModesState {
    // The last ground test: what it hit (none: no drawn surface under the camera, the height held), and what it tested.
    std::optional<glm::vec3> ground;
    std::string groundPrim;
    uint32_t groundChunks = 0;
    uint32_t groundDraws = 0;
    uint64_t groundTriangles = 0;
    float groundMs = 0.0f;
    // Orbiting: the pivot, from the depth under the cursor when Alt went down (a few frames later), and until then the point at the last
    // pivot's distance along the cursor ray.
    bool orbiting = false;
    glm::vec3 pivot = glm::vec3(0.0f);
    bool pivotPicked = false;
    float pivotDistanceMetres = 10.0f;
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
    CameraModesState modes;
};

void drawCameraWindow(bool& show, Camera& camera, CameraWindowState& state);
