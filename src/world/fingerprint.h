#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

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
