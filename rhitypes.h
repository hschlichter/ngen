#pragma once

#include <cstddef>
#include <cstdint>

enum class RhiBufferUsage : uint32_t {
    TransferSrc = 1 << 0,
    TransferDst = 1 << 1,
    Vertex = 1 << 2,
    Index = 1 << 3,
    Uniform = 1 << 4,
};

inline RhiBufferUsage operator|(RhiBufferUsage a, RhiBufferUsage b) {
    return (RhiBufferUsage) ((uint32_t) a | (uint32_t) b);
}

inline bool operator&(RhiBufferUsage a, RhiBufferUsage b) {
    return ((uint32_t) a & (uint32_t) b) != 0;
}

enum class RhiMemoryUsage {
    GpuOnly,
    CpuToGpu,
};

enum class RhiShaderStage : uint32_t {
    Vertex = 1 << 0,
    Fragment = 1 << 1,
};

enum class RhiFormat {
    R32G32_SFLOAT,
    R32G32B32_SFLOAT,
    R8G8B8A8_SRGB,
    D32_SFLOAT,
};

enum class RhiDescriptorType {
    UniformBuffer,
    CombinedImageSampler,
};

struct RhiExtent2D {
    uint32_t width, height;
};

struct RhiBufferDesc {
    uint64_t size;
    RhiBufferUsage usage;
    RhiMemoryUsage memory;
};

struct RhiTextureDesc {
    uint32_t width;
    uint32_t height;
    RhiFormat format;
    const void* initialData;
    uint64_t initialDataSize;
};

struct RhiSamplerDesc {
    // matches current usage: linear filter, repeat addressing
};

struct RhiVertexAttribute {
    uint32_t location;
    uint32_t binding;
    RhiFormat format;
    uint32_t offset;
};

struct RhiDescriptorBinding {
    uint32_t binding;
    RhiDescriptorType type;
    RhiShaderStage stage;
};

struct RhiPushConstantRange {
    RhiShaderStage stage;
    uint32_t offset;
    uint32_t size;
};

class RhiBuffer {
public:
    virtual ~RhiBuffer() = default;
};
class RhiTexture {
public:
    virtual ~RhiTexture() = default;
};
class RhiSampler {
public:
    virtual ~RhiSampler() = default;
};
class RhiShaderModule {
public:
    virtual ~RhiShaderModule() = default;
};
class RhiPipeline {
public:
    virtual ~RhiPipeline() = default;
};
class RhiDescriptorSetLayout {
public:
    virtual ~RhiDescriptorSetLayout() = default;
};
class RhiDescriptorPool {
public:
    virtual ~RhiDescriptorPool() = default;
};
class RhiDescriptorSet {
public:
    virtual ~RhiDescriptorSet() = default;
};
class RhiSemaphore {
public:
    virtual ~RhiSemaphore() = default;
};
class RhiFence {
public:
    virtual ~RhiFence() = default;
};
class RhiRenderPass {
public:
    virtual ~RhiRenderPass() = default;
};
class RhiFramebuffer {
public:
    virtual ~RhiFramebuffer() = default;
};

struct RhiGraphicsPipelineDesc {
    RhiShaderModule* vertexShader;
    RhiShaderModule* fragmentShader;
    RhiDescriptorSetLayout* descriptorSetLayout;
    RhiRenderPass* renderPass;
    uint32_t vertexStride;
    const RhiVertexAttribute* vertexAttributes;
    uint32_t vertexAttributeCount;
    RhiPushConstantRange pushConstant;
    RhiExtent2D viewportExtent;
};

struct RhiRenderPassBeginDesc {
    RhiRenderPass* renderPass;
    RhiFramebuffer* framebuffer;
    RhiExtent2D extent;
    float clearColor[4];
    float clearDepth;
};

struct RhiDescriptorWrite {
    uint32_t binding;
    RhiDescriptorType type;
    RhiBuffer* buffer;
    uint64_t bufferRange;
    RhiTexture* texture;
    RhiSampler* sampler;
};

struct RhiSubmitInfo {
    RhiSemaphore* waitSemaphore;
    RhiSemaphore* signalSemaphore;
    RhiFence* fence;
};
