#pragma once

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class ViewScene;
struct Camera;

// How a flight ended: the cameras it flew through, its metres and seconds, and whether it was stopped before the last camera.
struct CameraFlightEnd {
    size_t cameras = 0;
    double metres = 0.0;
    double seconds = 0.0;
    bool stopped = false;
};

// A camera flight (`fly`, view.camera.fly, the Streaming window): the camera moves through the scene's cameras whose paths start with a
// prefix, in path order, at a speed in metres a second of the frame's time, eye and direction interpolated between them. A test driver: it
// moves the camera, it decides nothing; a script holds its clock while it flies.
struct CameraFlight {
    std::vector<std::string> cameras;
    std::vector<glm::vec3> eyes;
    std::vector<float> yaws; // degrees
    std::vector<float> pitches;
    float speed = 0.0f; // metres a second
    size_t segment = 0; // from eyes[segment] to eyes[segment + 1]
    float along = 0.0f; // metres into the segment
    double metres = 0.0;
    double seconds = 0.0;
    std::function<void(const CameraFlightEnd&)> ended; // called once, when the last camera is reached or the flight is stopped

    auto flying() const -> bool { return segment + 1 < eyes.size(); }
};

// Starts a flight of `camera` through the cameras whose paths start with `prefix` at `speed` metres a second; `ended`, when given, is called
// when it ends. An error when fewer than two cameras match. A flight running before is stopped.
auto startFlight(CameraFlight& flight, Camera& camera, const ViewScene& scene, const std::string& prefix, float speed, std::function<void(const CameraFlightEnd&)> ended)
    -> std::optional<std::string>;
// Stops the flight where the camera is.
auto stopFlight(CameraFlight& flight) -> void;
// Moves the camera along its flight by a frame's time. Once a frame, before the camera is posted.
auto advanceFlight(CameraFlight& flight, Camera& camera, float dt) -> void;
