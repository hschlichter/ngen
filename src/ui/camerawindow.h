#pragma once

#include "camera.h"
#include "debugcamera.h"
#include "ground.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

// Camera window: current pose in the --camera flag syntax, jump, a flight through the cameras the scene authors, those cameras, and bookmarks kept in a
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
    // The flight through the scene's cameras (view.camera.fly): the prefix and speed the window flies with, what it asked for this frame
    // (the application starts and stops the flight and clears these), and the flight as the application last saw it.
    char flightPrefix[128] = "/cameras/flight_";
    float flightSpeed = 30.0f;
    bool flyRequested = false;
    bool flyStopRequested = false;
    bool flying = false;
    std::string flightText; // the flight's progress, or why it did not start
};

// The Camera window: the main camera's pose and the scene's cameras and bookmarks, the camera modes (`modes`, their switches by name) and
// the debug camera (`debug`, likewise).
void drawCameraWindow(bool& show, Camera& camera, CameraWindowState& state, DebugCameraState& debug, CameraModesState& modes);
