#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

enum class RhiBufferUsage : uint32_t {
    TransferSrc = 1 << 0,
    TransferDst = 1 << 1,
    Vertex = 1 << 2,
    Index = 1 << 3,
    Uniform = 1 << 4,
};

inline auto operator|(RhiBufferUsage a, RhiBufferUsage b) -> RhiBufferUsage {
    return (RhiBufferUsage) (std::to_underlying(a) | std::to_underlying(b));
}

inline auto operator&(RhiBufferUsage a, RhiBufferUsage b) -> bool {
    return (std::to_underlying(a) & std::to_underlying(b)) != 0;
}

enum class RhiMemoryUsage {
    GpuOnly,
    CpuToGpu,
};

enum class RhiShaderStage : uint32_t {
    Vertex = 1 << 0,
    Fragment = 1 << 1,
};

enum class RhiPrimitiveTopology {
    TriangleList,
    LineList,
};

enum class RhiFormat {
    Undefined,
    R32G32_SFLOAT,
    R32G32B32_SFLOAT,
    R8G8B8A8_SRGB,
    R8G8B8A8_UNORM,
    B8G8R8A8_SRGB,
    B8G8R8A8_UNORM,
    R32G32B32A32_SFLOAT,
    D32_SFLOAT,
};

enum class RhiDescriptorType {
    UniformBuffer,
    CombinedImageSampler,
};

enum class RhiTextureUsage : uint32_t {
    Sampled = 1 << 0,
    ColorAttachment = 1 << 1,
    DepthAttachment = 1 << 2,
    Storage = 1 << 3,
    TransferSrc = 1 << 4,
    TransferDst = 1 << 5,
};

inline auto operator|(RhiTextureUsage a, RhiTextureUsage b) -> RhiTextureUsage {
    return (RhiTextureUsage) (std::to_underlying(a) | std::to_underlying(b));
}

inline auto operator&(RhiTextureUsage a, RhiTextureUsage b) -> bool {
    return (std::to_underlying(a) & std::to_underlying(b)) != 0;
}

enum class RhiImageLayout {
    Undefined,
    ColorAttachment,
    DepthStencilAttachment,
    ShaderReadOnly,
    TransferSrc,
    TransferDst,
    PresentSrc,
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
    RhiTextureUsage usage = RhiTextureUsage::Sampled | RhiTextureUsage::TransferDst;
    const void* initialData = nullptr;
    uint64_t initialDataSize = 0;
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
    RhiBuffer() = default;
    RhiBuffer(const RhiBuffer&) = delete;
    RhiBuffer& operator=(const RhiBuffer&) = delete;
    RhiBuffer(RhiBuffer&&) = default;
    RhiBuffer& operator=(RhiBuffer&&) = default;
    virtual ~RhiBuffer() = default;
};
class RhiTexture {
public:
    RhiTexture() = default;
    RhiTexture(const RhiTexture&) = delete;
    RhiTexture& operator=(const RhiTexture&) = delete;
    RhiTexture(RhiTexture&&) = default;
    RhiTexture& operator=(RhiTexture&&) = default;
    virtual ~RhiTexture() = default;
};
class RhiSampler {
public:
    RhiSampler() = default;
    RhiSampler(const RhiSampler&) = delete;
    RhiSampler& operator=(const RhiSampler&) = delete;
    RhiSampler(RhiSampler&&) = default;
    RhiSampler& operator=(RhiSampler&&) = default;
    virtual ~RhiSampler() = default;
};
class RhiShaderModule {
public:
    RhiShaderModule() = default;
    RhiShaderModule(const RhiShaderModule&) = delete;
    RhiShaderModule& operator=(const RhiShaderModule&) = delete;
    RhiShaderModule(RhiShaderModule&&) = default;
    RhiShaderModule& operator=(RhiShaderModule&&) = default;
    virtual ~RhiShaderModule() = default;
};
class RhiPipeline {
public:
    RhiPipeline() = default;
    RhiPipeline(const RhiPipeline&) = delete;
    RhiPipeline& operator=(const RhiPipeline&) = delete;
    RhiPipeline(RhiPipeline&&) = default;
    RhiPipeline& operator=(RhiPipeline&&) = default;
    virtual ~RhiPipeline() = default;
};
class RhiDescriptorSetLayout {
public:
    RhiDescriptorSetLayout() = default;
    RhiDescriptorSetLayout(const RhiDescriptorSetLayout&) = delete;
    RhiDescriptorSetLayout& operator=(const RhiDescriptorSetLayout&) = delete;
    RhiDescriptorSetLayout(RhiDescriptorSetLayout&&) = default;
    RhiDescriptorSetLayout& operator=(RhiDescriptorSetLayout&&) = default;
    virtual ~RhiDescriptorSetLayout() = default;
};
class RhiDescriptorPool {
public:
    RhiDescriptorPool() = default;
    RhiDescriptorPool(const RhiDescriptorPool&) = delete;
    RhiDescriptorPool& operator=(const RhiDescriptorPool&) = delete;
    RhiDescriptorPool(RhiDescriptorPool&&) = default;
    RhiDescriptorPool& operator=(RhiDescriptorPool&&) = default;
    virtual ~RhiDescriptorPool() = default;
};
class RhiDescriptorSet {
public:
    RhiDescriptorSet() = default;
    RhiDescriptorSet(const RhiDescriptorSet&) = delete;
    RhiDescriptorSet& operator=(const RhiDescriptorSet&) = delete;
    RhiDescriptorSet(RhiDescriptorSet&&) = default;
    RhiDescriptorSet& operator=(RhiDescriptorSet&&) = default;
    virtual ~RhiDescriptorSet() = default;
};
class RhiSemaphore {
public:
    RhiSemaphore() = default;
    RhiSemaphore(const RhiSemaphore&) = delete;
    RhiSemaphore& operator=(const RhiSemaphore&) = delete;
    RhiSemaphore(RhiSemaphore&&) = default;
    RhiSemaphore& operator=(RhiSemaphore&&) = default;
    virtual ~RhiSemaphore() = default;
};
class RhiFence {
public:
    RhiFence() = default;
    RhiFence(const RhiFence&) = delete;
    RhiFence& operator=(const RhiFence&) = delete;
    RhiFence(RhiFence&&) = default;
    RhiFence& operator=(RhiFence&&) = default;
    virtual ~RhiFence() = default;
};

struct RhiRenderingAttachmentInfo {
    RhiTexture* texture = nullptr;
    RhiImageLayout layout = RhiImageLayout::Undefined;
    bool clear = false;
    std::array<float, 4> clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
    float clearDepth = 1.0f;
};

struct RhiRenderingInfo {
    RhiExtent2D extent;
    std::span<const RhiRenderingAttachmentInfo> colorAttachments;
    const RhiRenderingAttachmentInfo* depthAttachment = nullptr;
};

struct RhiBarrierDesc {
    RhiTexture* texture;
    RhiImageLayout oldLayout;
    RhiImageLayout newLayout;
};

struct RhiGraphicsPipelineDesc {
    RhiShaderModule* vertexShader;
    RhiShaderModule* fragmentShader;
    RhiDescriptorSetLayout* descriptorSetLayout;
    std::span<const RhiFormat> colorFormats;
    RhiFormat depthFormat = RhiFormat::Undefined;
    uint32_t vertexStride;
    std::span<const RhiVertexAttribute> vertexAttributes;
    RhiPushConstantRange pushConstant;
    RhiExtent2D viewportExtent;
    RhiPrimitiveTopology topology = RhiPrimitiveTopology::TriangleList;
    bool depthTestEnable = true;
    bool depthWriteEnable = true;
    bool backfaceCulling = true;
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
