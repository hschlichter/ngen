#include "cameraflight.h"

#include "camera.h"
#include "trace.h"
#include "viewscene.h"

#include <algorithm>
#include <cmath>
#include <format>

auto startFlight(CameraFlight& flight, Camera& camera, const ViewScene& scene, const std::string& prefix, float speed, std::function<void(const CameraFlightEnd&)> ended)
    -> std::optional<std::string> {
    std::vector<std::pair<std::string, glm::mat4>> found;
    for (const auto& authored : scene.cameras()) {
        if (authored.path.starts_with(prefix)) {
            found.emplace_back(authored.path, authored.worldTransform);
        }
    }
    std::ranges::sort(found, {}, [](const auto& authored) { return authored.first; });
    if (found.size() < 2) {
        return std::format("{} cameras start with '{}': a flight needs two", found.size(), prefix);
    }
    stopFlight(flight);
    flight.speed = speed;
    for (const auto& [path, transform] : found) {
        Camera placed = camera;
        placed.place(transform);
        flight.cameras.push_back(path);
        flight.eyes.push_back(placed.position);
        flight.yaws.push_back(placed.yaw);
        flight.pitches.push_back(placed.pitch);
    }
    flight.ended = std::move(ended);
    camera.position = flight.eyes.front();
    camera.yaw = flight.yaws.front();
    camera.pitch = flight.pitches.front();
    TRACE_EVENT("Engine", "FlightStarted", prefix).text(std::format("flying {} cameras from {} at {:g} m/s", found.size(), found.front().first, speed));
    return std::nullopt;
}

auto stopFlight(CameraFlight& flight) -> void {
    if (flight.ended) {
        flight.ended({.cameras = flight.cameras.size(), .metres = flight.metres, .seconds = flight.seconds, .stopped = true});
    }
    flight = {};
}

auto advanceFlight(CameraFlight& flight, Camera& camera, float dt) -> void {
    if (!flight.flying()) {
        return;
    }
    flight.seconds += dt;
    float step = flight.speed * dt;
    while (step > 0.0f && flight.flying()) {
        float length = glm::distance(flight.eyes[flight.segment], flight.eyes[flight.segment + 1]);
        if (flight.along + step < length) {
            flight.along += step;
            flight.metres += step;
            step = 0.0f;
        } else {
            flight.metres += length - flight.along;
            step -= length - flight.along;
            flight.segment++;
            flight.along = 0.0f;
        }
    }
    if (flight.flying()) {
        auto from = flight.segment;
        float length = glm::distance(flight.eyes[from], flight.eyes[from + 1]);
        float t = length > 0.0f ? flight.along / length : 1.0f;
        // Yaw the short way round.
        float turn = std::remainder(flight.yaws[from + 1] - flight.yaws[from], 360.0f);
        camera.position = glm::mix(flight.eyes[from], flight.eyes[from + 1], t);
        camera.yaw = flight.yaws[from] + (turn * t);
        camera.pitch = glm::mix(flight.pitches[from], flight.pitches[from + 1], t);
        return;
    }
    camera.position = flight.eyes.back();
    camera.yaw = flight.yaws.back();
    camera.pitch = flight.pitches.back();
    TRACE_EVENT("Engine", "FlightDone", flight.cameras.back())
        .text(std::format("flew {} cameras, {:.0f} m in {:.1f} s", flight.cameras.size(), flight.metres, flight.seconds))
        .field("metres", flight.metres)
        .field("seconds", flight.seconds);
    if (flight.ended) {
        flight.ended({.cameras = flight.cameras.size(), .metres = flight.metres, .seconds = flight.seconds, .stopped = false});
        flight.ended = nullptr;
    }
}
