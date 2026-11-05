#include "capturefiles.h"

#include "framegraphdebug.h"
#include "gpuschema.h"
#include "screenshot.h"

#include <algorithm>
#include <cstdio>

namespace {

auto jsonEscape(const std::string& text) -> std::string {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

} // namespace

auto writeCaptureJson(FILE* f, const CaptureResult& r, size_t maxRows) -> void {
    if (r.texture) {
        std::fprintf(f, "{\"frame\": %llu, \"pass\": \"%s\", \"resource\": \"%s\", \"kind\": \"texture\", \"format\": \"%s\", \"width\": %u, \"height\": %u, \"bytes\": %llu, \"error\": \"%s\",\n", (unsigned long long) r.frame, jsonEscape(r.pass).c_str(), jsonEscape(r.resource).c_str(), toString(r.format), r.width, r.height, (unsigned long long) r.byteSize, jsonEscape(r.error).c_str());
        std::fprintf(f, " \"regionX\": %u, \"regionY\": %u,\n", r.regionX, r.regionY);
        // Small captures (a cursor readout) list every texel's decoded value.
        auto texel = captureTexelBytes(r.format);
        if (r.bytes && texel > 0 && (uint64_t) r.width * r.height <= 64) {
            std::fprintf(f, " \"texels\": [");
            for (uint32_t i = 0; i < r.width * r.height; i++) {
                auto v = decodeTexel(r.format, r.bytes->data() + (size_t) i * texel);
                std::fprintf(f, "%s{\"x\": %u, \"y\": %u, \"value\": [%g, %g, %g, %g]}", i > 0 ? ", " : "", r.regionX + i % r.width, r.regionY + i / r.width, v.x, v.y, v.z, v.w);
            }
            std::fprintf(f, "],\n");
        }
        std::fprintf(f, " \"channelMin\": [%g, %g, %g, %g], \"channelMax\": [%g, %g, %g, %g]}\n", r.channelMin.x, r.channelMin.y, r.channelMin.z, r.channelMin.w, r.channelMax.x, r.channelMax.y, r.channelMax.z, r.channelMax.w);
        return;
    }
    const auto& schema = schemaForResource(r.resource);
    std::fprintf(f, "{\"frame\": %llu, \"pass\": \"%s\", \"resource\": \"%s\", \"kind\": \"buffer\", \"bytes\": %llu, \"error\": \"%s\",\n", (unsigned long long) r.frame, jsonEscape(r.pass).c_str(), jsonEscape(r.resource).c_str(), (unsigned long long) r.byteSize, jsonEscape(r.error).c_str());
    std::fprintf(f, " \"schema\": {\"name\": \"%s\", \"stride\": %u, \"fields\": [", schema.name.c_str(), schema.stride);
    for (size_t i = 0; i < schema.fields.size(); i++) {
        const auto& field = schema.fields[i];
        std::fprintf(f, "%s{\"name\": \"%s\", \"offset\": %u, \"type\": \"%s\"}", i > 0 ? ", " : "", field.name.c_str(), field.offset, fieldTypeName(field.type));
    }
    size_t rows = 0;
    if (r.bytes && schema.stride > 0) {
        rows = std::min(maxRows, r.bytes->size() / schema.stride);
    }
    std::fprintf(f, "]},\n \"rows\": [\n");
    for (size_t row = 0; row < rows; row++) {
        auto values = decodeRow(schema, *r.bytes, row);
        std::fprintf(f, "  {\"row\": %zu", row);
        for (size_t i = 0; i < values.size(); i++) {
            std::fprintf(f, ", \"%s\": \"%s\"", schema.fields[i].name.c_str(), values[i].c_str());
        }
        std::fprintf(f, "}%s\n", row + 1 < rows ? "," : "");
    }
    std::fprintf(f, " ]}\n");
}

auto writeCaptureFiles(const std::string& path, const CaptureResult& r, const CaptureDisplay& display, size_t maxRows) -> bool {
    if (r.texture) {
        uint32_t w = 0;
        uint32_t h = 0;
        auto rgba = convertCaptureForDisplay(r, display, 0, w, h);
        if (!rgba.empty() && !writeScreenshotPng(path.c_str(), rgba, w, h)) {
            return false;
        }
    }
    // Textures: the PNG above plus `<path>.json`. Buffers: the rows as `<path>`.
    auto jsonPath = r.texture ? path + ".json" : path;
    auto* f = std::fopen(jsonPath.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    writeCaptureJson(f, r, maxRows);
    std::fclose(f);
    return true;
}
