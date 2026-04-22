#include "primshapemesh.h"

#include <cmath>
#include <string_view>

namespace {

constexpr float kPi = 3.14159265358979323846f;

// "X" / "Y" / "Z" → axis vector; anything else → +Z. USD's axis attribute on
// Cylinder / Cone returns one of these three tokens.
void axisVectors(std::string_view axis, std::array<float, 3>& out_axis, std::array<float, 3>& out_side0, std::array<float, 3>& out_side1) {
    if (axis == "X") {
        out_axis = {1, 0, 0};
        out_side0 = {0, 1, 0};
        out_side1 = {0, 0, 1};
    } else if (axis == "Y") {
        out_axis = {0, 1, 0};
        out_side0 = {0, 0, 1};
        out_side1 = {1, 0, 0};
    } else {
        // Z and fallback
        out_axis = {0, 0, 1};
        out_side0 = {1, 0, 0};
        out_side1 = {0, 1, 0};
    }
}

inline void pushVertex(MeshDesc& out, std::array<float, 3> p, std::array<float, 3> n, ShapeColor color) {
    Vertex v{};
    v.position = p;
    v.normal = n;
    v.color = {color.r, color.g, color.b};
    v.texCoord = {0.0f, 0.0f};
    out.vertices.push_back(v);
}

inline void pushTri(MeshDesc& out, uint32_t a, uint32_t b, uint32_t c) {
    out.indices.push_back(a);
    out.indices.push_back(b);
    out.indices.push_back(c);
}

} // namespace

// Cube: 24 verts (4 per face, one face per flat normal) + 36 indices. UsdGeomCube's
// `size` is edge length along each axis, centered on origin.
MeshDesc tessellateCube(double size, ShapeColor color) {
    MeshDesc out;
    float h = static_cast<float>(size) * 0.5f;
    struct Face {
        std::array<float, 3> n;
        std::array<std::array<float, 3>, 4> corners; // CCW winding facing +n
    };
    const std::array<Face, 6> faces = {{
        {{+1, 0, 0}, {{{+h, -h, -h}, {+h, +h, -h}, {+h, +h, +h}, {+h, -h, +h}}}},
        {{-1, 0, 0}, {{{-h, -h, +h}, {-h, +h, +h}, {-h, +h, -h}, {-h, -h, -h}}}},
        {{0, +1, 0}, {{{-h, +h, -h}, {-h, +h, +h}, {+h, +h, +h}, {+h, +h, -h}}}},
        {{0, -1, 0}, {{{-h, -h, +h}, {-h, -h, -h}, {+h, -h, -h}, {+h, -h, +h}}}},
        {{0, 0, +1}, {{{+h, -h, +h}, {+h, +h, +h}, {-h, +h, +h}, {-h, -h, +h}}}},
        {{0, 0, -1}, {{{-h, -h, -h}, {-h, +h, -h}, {+h, +h, -h}, {+h, -h, -h}}}},
    }};
    for (const auto& f : faces) {
        auto base = static_cast<uint32_t>(out.vertices.size());
        for (const auto& c : f.corners) {
            pushVertex(out, c, f.n, color);
        }
        pushTri(out, base + 0, base + 1, base + 2);
        pushTri(out, base + 0, base + 2, base + 3);
    }
    return out;
}

// Sphere: UV-mapped lat/lon grid. Normals are radial. UsdGeomSphere's `radius`
// drives vertex positions; origin is center.
MeshDesc tessellateSphere(double radius, ShapeColor color, int latitudeSegments, int longitudeSegments) {
    MeshDesc out;
    float r = static_cast<float>(radius);
    int lat = latitudeSegments < 2 ? 2 : latitudeSegments;
    int lon = longitudeSegments < 3 ? 3 : longitudeSegments;

    // Vertex grid: (lat+1) rows × (lon+1) cols so the seam has separate verts
    // (different texcoord/normal not critical here, but keeps topology simple).
    for (int i = 0; i <= lat; i++) {
        float v = static_cast<float>(i) / static_cast<float>(lat);
        float phi = v * kPi; // 0..pi
        float sp = std::sin(phi);
        float cp = std::cos(phi);
        for (int j = 0; j <= lon; j++) {
            float u = static_cast<float>(j) / static_cast<float>(lon);
            float theta = u * 2.0f * kPi; // 0..2pi
            float st = std::sin(theta);
            float ct = std::cos(theta);
            std::array<float, 3> n = {sp * ct, cp, sp * st};
            std::array<float, 3> p = {r * n[0], r * n[1], r * n[2]};
            pushVertex(out, p, n, color);
        }
    }
    int stride = lon + 1;
    for (int i = 0; i < lat; i++) {
        for (int j = 0; j < lon; j++) {
            uint32_t a = static_cast<uint32_t>(i * stride + j);
            uint32_t b = static_cast<uint32_t>((i + 1) * stride + j);
            uint32_t c = static_cast<uint32_t>((i + 1) * stride + j + 1);
            uint32_t d = static_cast<uint32_t>(i * stride + j + 1);
            pushTri(out, a, b, c);
            pushTri(out, a, c, d);
        }
    }
    return out;
}

// Cylinder: caps + side band. USD's `height` is total length along the axis;
// origin is the centroid.
MeshDesc tessellateCylinder(double radius, double height, const char* axis, ShapeColor color) {
    MeshDesc out;
    float r = static_cast<float>(radius);
    float h = static_cast<float>(height) * 0.5f;
    std::array<float, 3> ax{}, s0{}, s1{};
    axisVectors(axis ? axis : "Z", ax, s0, s1);

    const int segments = 32;

    // Side band: two verts per segment × 2 caps.
    auto sideBase = static_cast<uint32_t>(out.vertices.size());
    for (int i = 0; i <= segments; i++) {
        float t = static_cast<float>(i) / static_cast<float>(segments) * 2.0f * kPi;
        float ct = std::cos(t);
        float st = std::sin(t);
        std::array<float, 3> n = {s0[0] * ct + s1[0] * st, s0[1] * ct + s1[1] * st, s0[2] * ct + s1[2] * st};
        std::array<float, 3> bottom = {n[0] * r - ax[0] * h, n[1] * r - ax[1] * h, n[2] * r - ax[2] * h};
        std::array<float, 3> top = {n[0] * r + ax[0] * h, n[1] * r + ax[1] * h, n[2] * r + ax[2] * h};
        pushVertex(out, bottom, n, color);
        pushVertex(out, top, n, color);
    }
    for (int i = 0; i < segments; i++) {
        uint32_t b0 = sideBase + static_cast<uint32_t>(i * 2);
        uint32_t t0 = b0 + 1;
        uint32_t b1 = sideBase + static_cast<uint32_t>((i + 1) * 2);
        uint32_t t1 = b1 + 1;
        pushTri(out, b0, t0, t1);
        pushTri(out, b0, t1, b1);
    }

    // Caps: center vertex + fan around the rim.
    auto topCenter = static_cast<uint32_t>(out.vertices.size());
    pushVertex(out, {ax[0] * h, ax[1] * h, ax[2] * h}, ax, color);
    auto topRimBase = static_cast<uint32_t>(out.vertices.size());
    for (int i = 0; i <= segments; i++) {
        float t = static_cast<float>(i) / static_cast<float>(segments) * 2.0f * kPi;
        float ct = std::cos(t);
        float st = std::sin(t);
        std::array<float, 3> p = {
            (s0[0] * ct + s1[0] * st) * r + ax[0] * h, (s0[1] * ct + s1[1] * st) * r + ax[1] * h, (s0[2] * ct + s1[2] * st) * r + ax[2] * h};
        pushVertex(out, p, ax, color);
    }
    for (int i = 0; i < segments; i++) {
        pushTri(out, topCenter, topRimBase + static_cast<uint32_t>(i), topRimBase + static_cast<uint32_t>(i + 1));
    }

    std::array<float, 3> naxNeg = {-ax[0], -ax[1], -ax[2]};
    auto botCenter = static_cast<uint32_t>(out.vertices.size());
    pushVertex(out, {-ax[0] * h, -ax[1] * h, -ax[2] * h}, naxNeg, color);
    auto botRimBase = static_cast<uint32_t>(out.vertices.size());
    for (int i = 0; i <= segments; i++) {
        float t = static_cast<float>(i) / static_cast<float>(segments) * 2.0f * kPi;
        float ct = std::cos(t);
        float st = std::sin(t);
        std::array<float, 3> p = {
            (s0[0] * ct + s1[0] * st) * r - ax[0] * h, (s0[1] * ct + s1[1] * st) * r - ax[1] * h, (s0[2] * ct + s1[2] * st) * r - ax[2] * h};
        pushVertex(out, p, naxNeg, color);
    }
    for (int i = 0; i < segments; i++) {
        // Reverse winding so bottom faces out.
        pushTri(out, botCenter, botRimBase + static_cast<uint32_t>(i + 1), botRimBase + static_cast<uint32_t>(i));
    }

    return out;
}

// Cone: base cap + side. USD's cone has apex at +axis*height/2, base at -axis*height/2.
MeshDesc tessellateCone(double radius, double height, const char* axis, ShapeColor color) {
    MeshDesc out;
    float r = static_cast<float>(radius);
    float h = static_cast<float>(height) * 0.5f;
    std::array<float, 3> ax{}, s0{}, s1{};
    axisVectors(axis ? axis : "Z", ax, s0, s1);

    const int segments = 32;

    // Side: per-segment normal approximated (not perpendicular to slant, but
    // good enough for a debug visual). Using radial direction keeps lighting
    // roughly correct for thin cones.
    auto sideBase = static_cast<uint32_t>(out.vertices.size());
    for (int i = 0; i <= segments; i++) {
        float t = static_cast<float>(i) / static_cast<float>(segments) * 2.0f * kPi;
        float ct = std::cos(t);
        float st = std::sin(t);
        std::array<float, 3> n = {s0[0] * ct + s1[0] * st, s0[1] * ct + s1[1] * st, s0[2] * ct + s1[2] * st};
        std::array<float, 3> base = {n[0] * r - ax[0] * h, n[1] * r - ax[1] * h, n[2] * r - ax[2] * h};
        std::array<float, 3> apex = {ax[0] * h, ax[1] * h, ax[2] * h};
        pushVertex(out, base, n, color);
        pushVertex(out, apex, n, color);
    }
    for (int i = 0; i < segments; i++) {
        uint32_t b0 = sideBase + static_cast<uint32_t>(i * 2);
        uint32_t a0 = b0 + 1;
        uint32_t b1 = sideBase + static_cast<uint32_t>((i + 1) * 2);
        pushTri(out, b0, a0, b1);
    }

    // Base cap.
    std::array<float, 3> nAxNeg = {-ax[0], -ax[1], -ax[2]};
    auto baseCenter = static_cast<uint32_t>(out.vertices.size());
    pushVertex(out, {-ax[0] * h, -ax[1] * h, -ax[2] * h}, nAxNeg, color);
    auto baseRimBase = static_cast<uint32_t>(out.vertices.size());
    for (int i = 0; i <= segments; i++) {
        float t = static_cast<float>(i) / static_cast<float>(segments) * 2.0f * kPi;
        float ct = std::cos(t);
        float st = std::sin(t);
        std::array<float, 3> p = {
            (s0[0] * ct + s1[0] * st) * r - ax[0] * h, (s0[1] * ct + s1[1] * st) * r - ax[1] * h, (s0[2] * ct + s1[2] * st) * r - ax[2] * h};
        pushVertex(out, p, nAxNeg, color);
    }
    for (int i = 0; i < segments; i++) {
        pushTri(out, baseCenter, baseRimBase + static_cast<uint32_t>(i + 1), baseRimBase + static_cast<uint32_t>(i));
    }

    return out;
}
