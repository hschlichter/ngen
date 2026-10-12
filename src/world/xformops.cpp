#include "xformops.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>

namespace {

auto vec3(const PackValue& value) -> std::optional<glm::dvec3> {
    if (value.numbers.size() < 3) {
        return std::nullopt;
    }
    return glm::dvec3(value.numbers[0], value.numbers[1], value.numbers[2]);
}

auto rotation(char axis, double degrees) -> glm::dmat4 {
    glm::dvec3 direction(0.0);
    if (axis == 'X') {
        direction.x = 1.0;
    } else if (axis == 'Y') {
        direction.y = 1.0;
    } else {
        direction.z = 1.0;
    }
    return glm::rotate(glm::dmat4(1.0), glm::radians(degrees), direction);
}

// One op's matrix; nullopt when the type isn't one USD defines or the value doesn't fit it.
auto opMatrix(const std::string& type, const PackValue& value) -> std::optional<glm::dmat4> {
    if (type == "translate") {
        auto v = vec3(value);
        if (!v) {
            return std::nullopt;
        }
        return glm::translate(glm::dmat4(1.0), *v);
    }
    if (type == "scale") {
        auto v = vec3(value);
        if (!v) {
            return std::nullopt;
        }
        return glm::scale(glm::dmat4(1.0), *v);
    }
    if (type == "rotateX" || type == "rotateY" || type == "rotateZ") {
        if (value.numbers.empty()) {
            return std::nullopt;
        }
        return rotation(type.back(), value.numbers[0]);
    }
    // rotateABC rotates about A first, then B, then C.
    if (type.size() == 9 && type.starts_with("rotate")) {
        auto v = vec3(value);
        if (!v) {
            return std::nullopt;
        }
        glm::dmat4 m(1.0);
        for (int i = 2; i >= 0; i--) {
            char axis = type[6 + i];
            if (axis != 'X' && axis != 'Y' && axis != 'Z') {
                return std::nullopt;
            }
            int component = axis - 'X';
            m = m * rotation(axis, (*v)[component]);
        }
        return m;
    }
    if (type == "orient") {
        if (value.numbers.size() < 4) {
            return std::nullopt;
        }
        glm::dquat q(value.numbers[0], value.numbers[1], value.numbers[2], value.numbers[3]);
        return glm::mat4_cast(glm::normalize(q));
    }
    if (type == "transform") {
        if (value.numbers.size() < 16) {
            return std::nullopt;
        }
        glm::dmat4 m;
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                m[i][j] = value.numbers[(size_t) (i * 4 + j)];
            }
        }
        return m;
    }
    return std::nullopt;
}

} // namespace

auto evaluateXformOps(const std::vector<std::string>& order, const std::function<const PackValue*(const std::string&)>& attribute, bool& resetsXformStack)
    -> glm::dmat4 {
    resetsXformStack = false;
    glm::dmat4 local(1.0);
    for (size_t i = 0; i < order.size(); i++) {
        std::string name = order[i];
        if (name == "!resetXformStack!") {
            if (i == 0) {
                resetsXformStack = true;
            }
            continue;
        }
        bool invert = false;
        if (name.starts_with("!invert!")) {
            invert = true;
            name = name.substr(8);
        }
        if (!name.starts_with("xformOp:")) {
            continue;
        }
        // xformOp:<type>, or xformOp:<type>:<suffix>.
        auto type = name.substr(8);
        if (auto colon = type.find(':'); colon != std::string::npos) {
            type = type.substr(0, colon);
        }
        const PackValue* value = attribute(name);
        if (value == nullptr) {
            continue;
        }
        auto m = opMatrix(type, *value);
        if (!m) {
            continue;
        }
        if (invert) {
            *m = glm::inverse(*m);
        }
        local = local * *m;
    }
    return local;
}
