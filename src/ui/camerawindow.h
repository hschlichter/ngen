#pragma once

#include <string>
#include <vector>

struct Camera;

// Camera window: current pose in the --camera flag syntax, jump, the cameras the scene authors, and bookmarks kept in a
// text file next to the scene (one "name x y z yaw pitch" per line).
struct CameraBookmark {
    std::string name;
    float pose[5] = {};
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
};

void drawCameraWindow(bool& show, Camera& camera, CameraWindowState& state);
