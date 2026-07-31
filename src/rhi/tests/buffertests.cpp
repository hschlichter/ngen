#include "rhidevice.h"
#include "rhitestenv.h"
#include "rhitesthelpers.h"
#include "testharness.h"

#include <cstring>

NGEN_TEST("buffer-map-roundtrip") {
    auto* device = rhiTestDevice();
    constexpr uint64_t size = 256;
    auto pattern = makeBytePattern(size);

    auto* buffer = device->createBuffer({.size = size, .usage = RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::CpuToGpu});
    ctx.expect(buffer != nullptr, "createBuffer returned null");
    if (buffer == nullptr) {
        return;
    }

    void* mapped = device->mapBuffer(buffer);
    ctx.expect(mapped != nullptr, "mapBuffer returned null");
    if (mapped != nullptr) {
        memcpy(mapped, pattern.data(), size);
        device->unmapBuffer(buffer);

        mapped = device->mapBuffer(buffer);
        ctx.expect(mapped != nullptr, "second mapBuffer returned null");
        if (mapped != nullptr) {
            ctx.expect(memcmp(mapped, pattern.data(), size) == 0, "readback does not match the written pattern");
            device->unmapBuffer(buffer);
        }
    }

    device->destroyBuffer(buffer);
}

NGEN_TEST("buffer-copy") {
    auto* device = rhiTestDevice();
    constexpr uint64_t size = 4096;
    auto pattern = makeBytePattern(size);

    auto* src = device->createBuffer({.size = size, .usage = RhiBufferUsage::TransferSrc, .memory = RhiMemoryUsage::CpuToGpu});
    auto* dst = device->createBuffer({.size = size, .usage = RhiBufferUsage::TransferDst, .memory = RhiMemoryUsage::CpuToGpu});
    ctx.expect(src != nullptr, "createBuffer (src) returned null");
    ctx.expect(dst != nullptr, "createBuffer (dst) returned null");
    if (src == nullptr || dst == nullptr) {
        if (src != nullptr) {
            device->destroyBuffer(src);
        }
        if (dst != nullptr) {
            device->destroyBuffer(dst);
        }
        return;
    }

    void* mapped = device->mapBuffer(src);
    memcpy(mapped, pattern.data(), size);
    device->unmapBuffer(src);

    device->copyBuffer(src, dst, size);

    mapped = device->mapBuffer(dst);
    ctx.expect(memcmp(mapped, pattern.data(), size) == 0, "copied buffer does not match the source pattern");
    device->unmapBuffer(dst);

    device->destroyBuffer(src);
    device->destroyBuffer(dst);
}
