#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// One line of a scene fingerprint, as ngen-worldcheck (the World) and ngen-usdcheck (OpenUSD) print them, so the two compare as files:
// the prim's path, its type, and its point count (0 for a prim that isn't point-based), tab separated; with transforms, its world
// matrix, GfMatrix4d's elements [0][0] to [3][3], four decimals each. A fingerprint is every line, sorted.
//
// Header-only and C++20, as ngen-usdcheck builds as C++20 for OpenUSD.

inline auto fingerprintNumber(double value) -> std::string {
    auto text = std::format("{:.4f}", value);
    if (text == "-0.0000") {
        return "0.0000";
    }
    return text;
}

// `matrix` is null for no transform column, else 16 elements, [i][j] at i * 4 + j.
inline auto fingerprintLine(std::string_view path, std::string_view type, int64_t points, const double* matrix) -> std::string {
    auto line = std::format("{}\t{}\t{}", path, type, points);
    if (matrix != nullptr) {
        line += "\t";
        for (int i = 0; i < 16; i++) {
            if (i > 0) {
                line += " ";
            }
            line += fingerprintNumber(matrix[i]);
        }
    }
    return line;
}

// One point of a PointInstancer, as --instancers prints them: the instancer's path and the point's id (its authored id, or its index),
// the prototype it places (empty when the prototype isn't there), whether it is shown (0 when invisibleIds hides it), and its world matrix
// (the instancer's world transform times the point's translate * rotate * scale, the prototype's own transform not included), as
// fingerprintLine prints a matrix.
inline auto fingerprintPointLine(std::string_view instancer, int64_t id, std::string_view prototype, bool visible, const double* matrix) -> std::string {
    auto line = std::format("{}#{}\t{}\t{}\t", instancer, id, prototype, visible ? 1 : 0);
    for (int i = 0; i < 16; i++) {
        if (i > 0) {
            line += " ";
        }
        line += fingerprintNumber(matrix[i]);
    }
    return line;
}

// One heuristic of a LOD root, as --lod-heuristics prints them: the root's path and the heuristic's place among its lod:heuristics targets that
// author thresholds, its type and domain, its thresholds and blend thresholds, and its bounds as UsdLod's queries read them: a distance
// heuristic's center, then its boundingVolume's extent when that is valid; a screen-size heuristic's extent (its boundingVolume's, else its
// own, else -1..1). Numbers as fingerprintNumber prints them, space separated.
inline auto fingerprintHeuristicLine(std::string_view root, size_t index, std::string_view type, std::string_view domain, const std::vector<double>& thresholds, const std::vector<double>& blendThresholds, const std::vector<double>& bounds) -> std::string {
    auto numbers = [](const std::vector<double>& values) {
        std::string text;
        for (size_t i = 0; i < values.size(); i++) {
            if (i > 0) {
                text += " ";
            }
            text += fingerprintNumber(values[i]);
        }
        return text;
    };
    return std::format("{}#{}\t{}\t{}\t{}\t{}\t{}", root, index, type, domain, numbers(thresholds), numbers(blendThresholds), numbers(bounds));
}
