#include "viewcommands.h"

#include "camera.h"
#include "sessionscript.h"
#include "viewscene.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <expected>
#include <format>
#include <sstream>

// The camera's commands: place, read, orbit and frame the main camera (or the debug camera, when the tools give one), the scene's authored
// cameras, the camera flight, and the follow mode's verb.

namespace {

using rpc::Json;

// "x,y,z,yaw,pitch" places the main camera; "x,y,z,yaw,pitch debug" the debug camera.
auto parseCamera(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string poseText;
    std::string camera;
    in >> poseText >> camera;
    float pose[5] = {};
    if (!parseCameraPose(poseText, pose) || (!camera.empty() && camera != "main" && camera != "debug")) {
        return std::unexpected(std::format("expected 'x,y,z,yaw,pitch [main|debug]', got '{}'", args));
    }
    Json out = {{"x", pose[0]}, {"y", pose[1]}, {"z", pose[2]}, {"yaw", pose[3]}, {"pitch", pose[4]}};
    if (!camera.empty()) {
        out["camera"] = camera;
    }
    return out;
}

auto parseTarget(const std::string& args) -> std::expected<Json, std::string> {
    return Json{{"target", args}};
}

auto parseCameraPath(const std::string& args) -> std::expected<Json, std::string> {
    return Json{{"camera", args}};
}

// `fly <camera path prefix> <speed m/s>`.
auto parseFly(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string prefix;
    float speed = 0.0f;
    if (!(in >> prefix >> speed) || speed <= 0.0f) {
        return std::unexpected(std::format("expected '<camera path prefix> <speed m/s>', got '{}'", args));
    }
    return Json{{"prefix", prefix}, {"speed", speed}};
}

// "x,y,z yaw pitch [main|debug]": turn a camera around the pivot x,y,z by the angles.
auto parseCameraOrbit(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string pivotText;
    float yaw = 0.0f;
    float pitch = 0.0f;
    std::string camera;
    in >> pivotText >> yaw >> pitch;
    bool parsed = !in.fail();
    in >> camera;
    std::array<float, 3> pivot = {};
    if (!parsed || std::sscanf(pivotText.c_str(), "%f,%f,%f", &pivot[0], &pivot[1], &pivot[2]) != 3 || (!camera.empty() && camera != "main" && camera != "debug")) {
        return std::unexpected(std::format("expected 'x,y,z yaw pitch [main|debug]', got '{}'", args));
    }
    Json out = {{"pivot", {pivot[0], pivot[1], pivot[2]}}, {"yaw", yaw}, {"pitch", pitch}};
    if (!camera.empty()) {
        out["camera"] = camera;
    }
    return out;
}

auto parseCameraFollow(const std::string& args) -> std::expected<Json, std::string> {
    std::istringstream in(args);
    std::string first;
    std::string metres;
    in >> first >> metres;
    if ((first == "on" || first == "off") && metres.empty()) {
        return configValues({{"view.camera.follow", first}});
    }
    if (first == "height" && !metres.empty()) {
        return configValues({{"view.camera.followHeight", metres}});
    }
    return std::unexpected(std::format("expected 'on|off' or 'height <metres>', got '{}'", args));
}

// The camera a `camera` parameter names: the main camera unless it says "debug"; none for the debug camera while it is off.
auto namedCamera(ViewContext& view, const Json& p) -> Camera* {
    if (p.value("camera", std::string("main")) != "debug") {
        return &view.cam;
    }
    return view.debugCamera ? view.debugCamera() : nullptr;
}

const std::vector<std::string> cameraNames = {"main", "debug"};

} // namespace

auto registerCameraCommands(RpcRegistry& registry, RpcVerbs& verbs, ViewContext& view) -> void {
    registry.add(
        {
            .name = "view.camera.set",
            .summary = "Place a camera: position and yaw/pitch in degrees. The main camera (the player: residency, LOD and culling follow it) unless `camera` is debug.",
            .params = {
                requiredField("x", RpcType::Float, "position x"),
                requiredField("y", RpcType::Float, "position y"),
                requiredField("z", RpcType::Float, "position z"),
                requiredField("yaw", RpcType::Float, "degrees"),
                requiredField("pitch", RpcType::Float, "degrees"),
                optionalField("speed", RpcType::Float, "fly speed, m/s"),
                {.name = "camera", .type = RpcType::Enum, .required = false, .description = "which camera, main by default", .enumValues = cameraNames},
            },
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto* camera = namedCamera(view, p);
            if (camera == nullptr) {
                responder.fail(rpc::appError, "the debug camera is off (view.camera.debug {on: true})");
                return;
            }
            camera->position = glm::vec3(p["x"].get<float>(), p["y"].get<float>(), p["z"].get<float>());
            camera->yaw = p["yaw"].get<float>();
            camera->pitch = p["pitch"].get<float>();
            if (p.contains("speed")) {
                camera->speed = std::clamp(p["speed"].get<float>(), 0.1f, 5000.0f);
            }
            responder.respond(Json::object());
        });
    registry.add(
        {
            .name = "view.camera.get",
            .summary = "A camera's position and yaw/pitch: the main camera unless `camera` is debug.",
            .params = {{.name = "camera", .type = RpcType::Enum, .required = false, .description = "which camera, main by default", .enumValues = cameraNames}},
            .result = "{x, y, z, yaw, pitch}",
        },
        [&view](const Json& p, RpcResponder responder) {
            const auto* camera = namedCamera(view, p);
            if (camera == nullptr) {
                responder.fail(rpc::appError, "the debug camera is off (view.camera.debug {on: true})");
                return;
            }
            responder.respond({{"x", camera->position.x}, {"y", camera->position.y}, {"z", camera->position.z}, {"yaw", camera->yaw}, {"pitch", camera->pitch}});
        });
    registry.add(
        {
            .name = "view.camera.orbit",
            .summary = "Turn a camera around a pivot by angles, as Alt + right-drag does: yaw around the world's up through the pivot, pitch around the camera's right, the distance kept. The main camera unless `camera` is debug.",
            .params = {requiredField("pivot", RpcType::Vec3, "the point to turn around, world"),
                       requiredField("yaw", RpcType::Float, "degrees"),
                       requiredField("pitch", RpcType::Float, "degrees"),
                       {.name = "camera", .type = RpcType::Enum, .required = false, .description = "which camera, main by default", .enumValues = cameraNames}},
            .result = "{x, y, z, yaw, pitch}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto* camera = namedCamera(view, p);
            if (camera == nullptr) {
                responder.fail(rpc::appError, "the debug camera is off (view.camera.debug {on: true})");
                return;
            }
            auto pivot = glm::vec3(p["pivot"][0].get<float>(), p["pivot"][1].get<float>(), p["pivot"][2].get<float>());
            camera->orbit(pivot, p["yaw"].get<float>(), p["pitch"].get<float>());
            responder.respond({{"x", camera->position.x}, {"y", camera->position.y}, {"z", camera->position.z}, {"yaw", camera->yaw}, {"pitch", camera->pitch}});
        });
    registry.add({.name = "view.camera.list", .summary = "The cameras the scene authors, in scene order.", .result = "{cameras: [{path, x, y, z, yaw, pitch}]}"},
                 [&view](const Json&, RpcResponder responder) {
                     auto list = Json::array();
                     for (const auto& camera : view.scene.cameras()) {
                         Camera placed = view.cam;
                         placed.place(camera.worldTransform);
                         list.push_back({{"path", camera.path}, {"x", placed.position.x}, {"y", placed.position.y}, {"z", placed.position.z}, {"yaw", placed.yaw}, {"pitch", placed.pitch}});
                     }
                     responder.respond({{"cameras", list}});
                 });
    registry.add(
        {
            .name = "view.camera.use",
            .summary = "Place the camera where a camera the scene authors is, looking where it looks (its roll is dropped).",
            .params = {requiredField("camera", RpcType::String, "the camera's prim path")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto path = p["camera"].get<std::string>();
            for (const auto& camera : view.scene.cameras()) {
                if (camera.path == path) {
                    view.cam.place(camera.worldTransform);
                    responder.respond(Json::object());
                    return;
                }
            }
            responder.fail(rpc::appError, std::format("the scene authors no camera at '{}'", path));
        });
    registry.add(
        {
            .name = "view.camera.fly",
            .summary = "Fly the camera through the cameras the scene authors whose paths start with a prefix, in path order (/cameras/flight_00, …), at a speed in metres a second, eye and direction interpolated. A test driver for streaming: a script holds its clock while it flies. Answers when the last camera is reached (or the flight is stopped), with the cameras, metres and seconds flown.",
            .params = {requiredField("prefix", RpcType::String, "a camera path prefix"), requiredField("speed", RpcType::Float, "metres a second")},
            .result = "{cameras, metres, seconds, stopped}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto speed = p["speed"].get<float>();
            if (speed <= 0.0f) {
                responder.fail(rpc::invalidParams, "speed is metres a second, above 0");
                return;
            }
            auto ended = [responder](const CameraFlightEnd& end) mutable {
                responder.respond({{"cameras", end.cameras}, {"metres", end.metres}, {"seconds", end.seconds}, {"stopped", end.stopped}});
            };
            if (auto error = startFlight(view.flight, view.cam, view.scene, p["prefix"].get<std::string>(), speed, ended)) {
                responder.fail(rpc::appError, *error);
            }
        });
    registry.add({.name = "view.camera.fly.stop", .summary = "Stops the camera's flight where it is; its call answers.", .result = "{}"}, [&view](const Json&, RpcResponder responder) {
        stopFlight(view.flight);
        responder.respond(Json::object());
    });
    registry.add(
        {
            .name = "view.camera.frame",
            .summary = "Frame the whole scene, or what is drawn at and under a prim.",
            .params = {requiredField("target", RpcType::String, "\"scene\" or a prim path")},
            .result = "{}",
        },
        [&view](const Json& p, RpcResponder responder) {
            auto target = p["target"].get<std::string>();
            if (target == "scene") {
                view.frameSceneView();
                responder.respond(Json::object());
                return;
            }
            auto bb = view.scene.bounds(target);
            if (!bb.valid()) {
                responder.fail(rpc::appError, std::format("nothing is drawn at or under '{}'", target));
                return;
            }
            view.cam.frame(bb, glm::radians(45.0f));
            responder.respond(Json::object());
        });

    verbs.add("camera", "view.camera.set", parseCamera);
    verbs.add("camera-frame", "view.camera.frame", parseTarget);
    verbs.add("camera-use", "view.camera.use", parseCameraPath);
    verbs.add("camera-orbit", "view.camera.orbit", parseCameraOrbit);
    verbs.add("camera-follow", "config.set", parseCameraFollow);
    verbs.add("fly", "view.camera.fly", parseFly);
    verbs.add("fly-stop", "view.camera.fly.stop", RpcVerbs::none);
}
