#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

// Type-safe bit set over a flag enum. Opt an enum in by specialising RhiFlagEnum;
// then `E | E` yields RhiFlags<E>, and RhiFlags<E>::has(E) tests a bit. Plain
// `enum class` operators were dropped because `a & b` returning bool made
// `usage & (A | B)` silently mean "any of".
template <typename E>
struct RhiFlagEnum : std::false_type {};

template <typename E>
concept RhiFlagEnumType = RhiFlagEnum<E>::value;

template <RhiFlagEnumType E>
struct RhiFlags {
    using Underlying = std::underlying_type_t<E>;
    Underlying bits = 0;

    constexpr RhiFlags() = default;
    constexpr RhiFlags(E flag) : bits(std::to_underlying(flag)) {}

    [[nodiscard]] constexpr auto has(E flag) const -> bool { return (bits & std::to_underlying(flag)) != 0; }
    [[nodiscard]] constexpr auto any() const -> bool { return bits != 0; }

    constexpr auto operator|=(RhiFlags other) -> RhiFlags& {
        bits |= other.bits;
        return *this;
    }

    friend constexpr auto operator|(RhiFlags a, RhiFlags b) -> RhiFlags {
        RhiFlags result;
        result.bits = a.bits | b.bits;
        return result;
    }

    friend constexpr auto operator&(RhiFlags a, RhiFlags b) -> RhiFlags {
        RhiFlags result;
        result.bits = a.bits & b.bits;
        return result;
    }

    friend constexpr auto operator==(RhiFlags a, RhiFlags b) -> bool = default;
};

template <RhiFlagEnumType E>
constexpr auto operator|(E a, E b) -> RhiFlags<E> {
    return RhiFlags<E>(a) | RhiFlags<E>(b);
}

enum class RhiBufferUsage : uint32_t {
    TransferSrc = 1 << 0,
    TransferDst = 1 << 1,
    Vertex = 1 << 2,
    Index = 1 << 3,
    Uniform = 1 << 4,
};
template <>
struct RhiFlagEnum<RhiBufferUsage> : std::true_type {};
using RhiBufferUsageFlags = RhiFlags<RhiBufferUsage>;

enum class RhiMemoryUsage {
    GpuOnly,
    CpuToGpu,
};

enum class RhiShaderStage : uint32_t {
    Vertex = 1 << 0,
    Fragment = 1 << 1,
};
template <>
struct RhiFlagEnum<RhiShaderStage> : std::true_type {};
using RhiShaderStageFlags = RhiFlags<RhiShaderStage>;

// Backend-agnostic failure classes. Backends log the native error code before
// returning one of these; callers only branch on the class.
enum class RhiError {
    Failed,     // generic backend failure, details on stderr
    OutOfDate,  // swapchain no longer matches the surface; recreate it
    Suboptimal, // presentation still works but the swapchain should be recreated
    DeviceLost,
};

enum class RhiPrimitiveTopology {
    TriangleList,
    LineList,
};

enum class RhiFormat {
    Undefined,
    R8_UNORM,
    R8G8_UNORM,
    R8G8B8A8_SRGB,
    R8G8B8A8_UNORM,
    B8G8R8A8_SRGB,
    B8G8R8A8_UNORM,
    R16G16B16A16_SFLOAT,
    R32G32_SFLOAT,
    R32G32B32_SFLOAT,
    R32G32B32A32_SFLOAT,
    D32_SFLOAT,
    D24_UNORM_S8_UINT,
};

enum class RhiTextureDimension {
    Texture2D,
    Texture2DArray,
    TextureCube, // arrayLayers must be 6
};

enum class RhiFilter {
    Nearest,
    Linear,
};

enum class RhiMipmapMode {
    Nearest,
    Linear,
};

enum class RhiAddressMode {
    Repeat,
    MirroredRepeat,
    ClampToEdge,
    ClampToBorder,
};

enum class RhiIndexType {
    Uint16,
    Uint32,
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
template <>
struct RhiFlagEnum<RhiTextureUsage> : std::true_type {};
using RhiTextureUsageFlags = RhiFlags<RhiTextureUsage>;

enum class RhiCullMode {
    None,
    Front,
    Back,
};

enum class RhiFrontFace {
    CounterClockwise,
    Clockwise,
};

enum class RhiCompareOp {
    Never,
    Less,
    Equal,
    LessOrEqual,
    Greater,
    NotEqual,
    GreaterOrEqual,
    Always,
};

enum class RhiBlendFactor {
    Zero,
    One,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
};

enum class RhiBlendOp {
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max,
};

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
    RhiBufferUsageFlags usage;
    RhiMemoryUsage memory;
};

struct RhiTextureDesc {
    uint32_t width;
    uint32_t height;
    RhiFormat format;
    RhiTextureUsageFlags usage = RhiTextureUsage::Sampled | RhiTextureUsage::TransferDst;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    uint32_t sampleCount = 1;
    RhiTextureDimension dimension = RhiTextureDimension::Texture2D;
};

struct RhiBufferCopy {
    uint64_t srcOffset = 0;
    uint64_t dstOffset = 0;
    uint64_t size = 0;
};

// Whole mip level 0 of a 2D texture; tightly packed rows starting at bufferOffset.
struct RhiBufferTextureCopy {
    uint64_t bufferOffset = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Static device capabilities the renderer needs to size and validate its own
// resources. Filled once at init; read through RhiDevice::limits().
struct RhiDeviceLimits {
    uint64_t minUniformBufferOffsetAlignment = 0;
    uint32_t maxPushConstantSize = 0;
    float maxLineWidth = 1.0f;
    bool wideLines = false;
    bool samplerAnisotropy = false;
};

// Compiled shader bytecode in the backend's native format (SPIR-V for Vulkan,
// DXIL for D3D12, metallib for Metal). Loading and cross-compiling happen
// outside the RHI; `code` only needs to stay alive for the duration of the call.
struct RhiShaderDesc {
    RhiShaderStage stage;
    std::span<const std::byte> code;
    const char* entryPoint = "main";
};

struct RhiSamplerDesc {
    RhiFilter magFilter = RhiFilter::Linear;
    RhiFilter minFilter = RhiFilter::Linear;
    RhiMipmapMode mipmapMode = RhiMipmapMode::Linear;
    RhiAddressMode addressU = RhiAddressMode::Repeat;
    RhiAddressMode addressV = RhiAddressMode::Repeat;
    RhiAddressMode addressW = RhiAddressMode::Repeat;
    float maxAnisotropy = 0.0f; // 0 = off; clamped to the device limit, ignored when unsupported
    bool compareEnable = false;
    RhiCompareOp compareOp = RhiCompareOp::Always;
    float minLod = 0.0f;
    float maxLod = 1000.0f; // "no clamp"
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
    RhiShaderStageFlags stage;
};

struct RhiPushConstantRange {
    RhiShaderStageFlags stage;
    uint32_t offset = 0;
    uint32_t size = 0;
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
    RhiTexture* texture = nullptr;
    RhiImageLayout oldLayout = RhiImageLayout::Undefined;
    RhiImageLayout newLayout = RhiImageLayout::Undefined;
};

struct RhiRasterState {
    RhiCullMode cullMode = RhiCullMode::Back;
    RhiFrontFace frontFace = RhiFrontFace::CounterClockwise;
    float lineWidth = 1.0f; // Only honored when topology is LineList; requires wideLines feature for >1.
};

struct RhiDepthState {
    bool testEnable = true;
    bool writeEnable = true;
    RhiCompareOp compareOp = RhiCompareOp::Less;
};

// Defaults describe standard alpha blending; `enable` is off so pipelines opt in.
struct RhiBlendState {
    bool enable = false;
    RhiBlendFactor srcColor = RhiBlendFactor::SrcAlpha;
    RhiBlendFactor dstColor = RhiBlendFactor::OneMinusSrcAlpha;
    RhiBlendOp colorOp = RhiBlendOp::Add;
    RhiBlendFactor srcAlpha = RhiBlendFactor::One;
    RhiBlendFactor dstAlpha = RhiBlendFactor::OneMinusSrcAlpha;
    RhiBlendOp alphaOp = RhiBlendOp::Add;
};

struct RhiGraphicsPipelineDesc {
    RhiShaderModule* vertexShader = nullptr;
    RhiShaderModule* fragmentShader = nullptr;
    std::span<RhiDescriptorSetLayout* const> descriptorSetLayouts; // index in span = set index
    RhiPushConstantRange pushConstant;
    std::span<const RhiFormat> colorFormats;
    RhiFormat depthFormat = RhiFormat::Undefined;
    uint32_t vertexStride = 0;
    std::span<const RhiVertexAttribute> vertexAttributes;
    RhiPrimitiveTopology topology = RhiPrimitiveTopology::TriangleList;
    RhiRasterState raster;
    RhiDepthState depth;
    RhiBlendState blend; // applied to every color attachment
};

struct RhiDescriptorWrite {
    uint32_t binding = 0;
    RhiDescriptorType type = RhiDescriptorType::UniformBuffer;
    RhiBuffer* buffer = nullptr;
    uint64_t bufferOffset = 0;
    uint64_t bufferRange = 0;
    RhiTexture* texture = nullptr;
    RhiSampler* sampler = nullptr;
};

struct RhiSubmitInfo {
    RhiSemaphore* waitSemaphore = nullptr;
    RhiSemaphore* signalSemaphore = nullptr;
    RhiFence* fence = nullptr;
};
