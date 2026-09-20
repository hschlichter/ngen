#include "rhidevicevulkan.h"

#include <vulkan/vk_enum_string_helper.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <utility>
#include <vector>

auto RhiDeviceVulkan::toVkBufferUsage(RhiBufferUsageFlags usage) -> VkBufferUsageFlags {
    using enum RhiBufferUsage;
    VkBufferUsageFlags flags = 0;
    if (usage.has(TransferSrc)) {
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (usage.has(TransferDst)) {
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if (usage.has(Vertex)) {
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (usage.has(Index)) {
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (usage.has(Uniform)) {
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (usage.has(Storage)) {
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    return flags;
}

auto RhiDeviceVulkan::toVkMemoryProps(RhiMemoryUsage usage) -> VkMemoryPropertyFlags {
    using enum RhiMemoryUsage;
    switch (usage) {
        case GpuOnly:
            return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        case CpuToGpu:
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    return 0;
}

auto RhiDeviceVulkan::toVkFormat(RhiFormat format) -> VkFormat {
    using enum RhiFormat;
    switch (format) {
        case Undefined:
            return VK_FORMAT_UNDEFINED;
        case R8_UNORM:
            return VK_FORMAT_R8_UNORM;
        case R8G8_UNORM:
            return VK_FORMAT_R8G8_UNORM;
        case R16G16B16A16_SFLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case D24_UNORM_S8_UINT:
            return VK_FORMAT_D24_UNORM_S8_UINT;
        case D32_SFLOAT_S8_UINT:
            return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case R32G32_SFLOAT:
            return VK_FORMAT_R32G32_SFLOAT;
        case R32G32B32_SFLOAT:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case R8G8B8A8_SRGB:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case R8G8B8A8_UNORM:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case B8G8R8A8_SRGB:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case B8G8R8A8_UNORM:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case R32G32B32A32_SFLOAT:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case D32_SFLOAT:
            return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

auto RhiDeviceVulkan::toVkShaderStage(RhiShaderStageFlags stage) -> VkShaderStageFlags {
    VkShaderStageFlags flags = 0;
    if (stage.has(RhiShaderStage::Vertex)) {
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (stage.has(RhiShaderStage::Fragment)) {
        flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if (stage.has(RhiShaderStage::Compute)) {
        flags |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return flags;
}

static auto toVkDescriptorType(RhiDescriptorType type) -> VkDescriptorType {
    switch (type) {
        case RhiDescriptorType::UniformBuffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case RhiDescriptorType::CombinedImageSampler:
            return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case RhiDescriptorType::StorageBuffer:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case RhiDescriptorType::StorageImage:
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

static auto toVkCullMode(RhiCullMode mode) -> VkCullModeFlags {
    switch (mode) {
        case RhiCullMode::None:
            return VK_CULL_MODE_NONE;
        case RhiCullMode::Front:
            return VK_CULL_MODE_FRONT_BIT;
        case RhiCullMode::Back:
            return VK_CULL_MODE_BACK_BIT;
    }
    return VK_CULL_MODE_NONE;
}

static auto toVkFrontFace(RhiFrontFace face) -> VkFrontFace {
    switch (face) {
        case RhiFrontFace::CounterClockwise:
            return VK_FRONT_FACE_COUNTER_CLOCKWISE;
        case RhiFrontFace::Clockwise:
            return VK_FRONT_FACE_CLOCKWISE;
    }
    return VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

static auto toVkCompareOp(RhiCompareOp op) -> VkCompareOp {
    switch (op) {
        case RhiCompareOp::Never:
            return VK_COMPARE_OP_NEVER;
        case RhiCompareOp::Less:
            return VK_COMPARE_OP_LESS;
        case RhiCompareOp::Equal:
            return VK_COMPARE_OP_EQUAL;
        case RhiCompareOp::LessOrEqual:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case RhiCompareOp::Greater:
            return VK_COMPARE_OP_GREATER;
        case RhiCompareOp::NotEqual:
            return VK_COMPARE_OP_NOT_EQUAL;
        case RhiCompareOp::GreaterOrEqual:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case RhiCompareOp::Always:
            return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

static auto toVkBlendFactor(RhiBlendFactor factor) -> VkBlendFactor {
    switch (factor) {
        case RhiBlendFactor::Zero:
            return VK_BLEND_FACTOR_ZERO;
        case RhiBlendFactor::One:
            return VK_BLEND_FACTOR_ONE;
        case RhiBlendFactor::SrcColor:
            return VK_BLEND_FACTOR_SRC_COLOR;
        case RhiBlendFactor::OneMinusSrcColor:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case RhiBlendFactor::DstColor:
            return VK_BLEND_FACTOR_DST_COLOR;
        case RhiBlendFactor::OneMinusDstColor:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case RhiBlendFactor::SrcAlpha:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case RhiBlendFactor::OneMinusSrcAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case RhiBlendFactor::DstAlpha:
            return VK_BLEND_FACTOR_DST_ALPHA;
        case RhiBlendFactor::OneMinusDstAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

static auto toVkBlendOp(RhiBlendOp op) -> VkBlendOp {
    switch (op) {
        case RhiBlendOp::Add:
            return VK_BLEND_OP_ADD;
        case RhiBlendOp::Subtract:
            return VK_BLEND_OP_SUBTRACT;
        case RhiBlendOp::ReverseSubtract:
            return VK_BLEND_OP_REVERSE_SUBTRACT;
        case RhiBlendOp::Min:
            return VK_BLEND_OP_MIN;
        case RhiBlendOp::Max:
            return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

auto RhiDeviceVulkan::toVkImageUsage(RhiTextureUsageFlags usage) -> VkImageUsageFlags {
    VkImageUsageFlags flags = 0;
    if (usage.has(RhiTextureUsage::Sampled)) {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (usage.has(RhiTextureUsage::ColorAttachment)) {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (usage.has(RhiTextureUsage::DepthAttachment)) {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (usage.has(RhiTextureUsage::Storage)) {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (usage.has(RhiTextureUsage::TransferSrc)) {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (usage.has(RhiTextureUsage::TransferDst)) {
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    return flags;
}

auto RhiDeviceVulkan::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) -> uint32_t {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if (((typeFilter & (1 << i)) != 0u) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    std::println(stderr, "Failed to find suitable memory type");
    return UINT32_MAX;
}

static VKAPI_ATTR auto VKAPI_CALL debugMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* userData) -> VkBool32 {
    auto* self = static_cast<RhiDeviceVulkan*>(userData);
    self->onValidationMessage(severity, data->pMessage != nullptr ? data->pMessage : "");
    return VK_FALSE;
}

auto RhiDeviceVulkan::onValidationMessage(uint32_t severity, const char* message) -> void {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0u) {
        validationErrors++;
        std::println(stderr, "[vulkan validation] error: {}", message);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0u) {
        validationWarnings++;
        std::println(stderr, "[vulkan validation] warning: {}", message);
    }
}

auto RhiDeviceVulkan::init(const RhiWindow& window, const RhiDeviceOptions& options) -> std::expected<void, RhiError> {
    uint32_t apiVersion = VK_API_VERSION_1_0;
    auto result = vkEnumerateInstanceVersion(&apiVersion);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkEnumerateInstanceVersion failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(RhiError::Failed);
    }

    std::println("Vulkan API version: {}.{}.{}", VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion));

    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "ngen",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "Custom Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = apiVersion,
    };

    auto extensions = window.instanceExtensions;

    // VK_EXT_debug_utils gives command buffer labels for RenderDoc and validation
    // messages. Optional: skip silently when the loader does not offer it.
    bool debugUtilsAvailable = false;
    {
        uint32_t availableCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
        std::vector<VkExtensionProperties> available(availableCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available.data());
        for (const auto& ext : available) {
            if (strcmp(ext.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
                debugUtilsAvailable = true;
                break;
            }
        }
    }
    if (debugUtilsAvailable) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    auto extensionsCount = (uint32_t) extensions.size();
    for (const auto* extension : extensions) {
        std::println("{}", extension);
    }

    const char* validationLayers[] = {
        // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
        "VK_LAYER_KHRONOS_validation",
    };
    uint32_t validationLayersCount = 0;
    if (options.enableValidation) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
        bool layerFound = false;
        for (const auto& layer : layers) {
            if (strcmp(layer.layerName, validationLayers[0]) == 0) {
                layerFound = true;
                break;
            }
        }
        if (!layerFound || !debugUtilsAvailable) {
            std::println(stderr, "Validation requested but {} or {} is not available", validationLayers[0], VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            return std::unexpected(RhiError::Failed);
        }
        validationLayersCount = 1;
    }

    VkInstanceCreateInfo instanceCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
#ifdef __APPLE__
        .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
#else
        .flags = 0,
#endif
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = validationLayersCount,
        .ppEnabledLayerNames = validationLayers,
        .enabledExtensionCount = extensionsCount,
        .ppEnabledExtensionNames = extensions.data(),
    };

    result = vkCreateInstance(&instanceCreateInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateInstance failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(RhiError::Failed);
    }

    if (debugUtilsAvailable) {
        cmdBeginLabelFn = (PFN_vkCmdBeginDebugUtilsLabelEXT) vkGetInstanceProcAddr(instance, "vkCmdBeginDebugUtilsLabelEXT");
        cmdEndLabelFn = (PFN_vkCmdEndDebugUtilsLabelEXT) vkGetInstanceProcAddr(instance, "vkCmdEndDebugUtilsLabelEXT");
    }

    if (options.enableValidation) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debugMessengerCallback,
            .pUserData = this,
        };
        auto createMessenger = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (createMessenger == nullptr || createMessenger(instance, &messengerInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
            std::println(stderr, "vkCreateDebugUtilsMessengerEXT failed");
            return std::unexpected(RhiError::Failed);
        }
    }

    if (!window.createSurface || !window.createSurface(instance, (void**) &surface)) {
        std::println(stderr, "RhiWindow::createSurface failed");
        return std::unexpected(RhiError::Failed);
    }

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkEnumeratePhysicalDevices failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(RhiError::Failed);
    }

    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data());

    for (uint32_t i = 0; i < deviceCount; i++) {
        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[i], &queueCount, nullptr);

        std::vector<VkQueueFamilyProperties> props(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[i], &queueCount, props.data());

        for (uint32_t j = 0; j < queueCount; j++) {
            uint32_t presentSupport = 0;
            result = vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevices[i], j, surface, &presentSupport);
            if (result != VK_SUCCESS) {
                std::println(stderr, "vkGetPhysicalDeviceSurfaceSupportKHR failed: {}({})", string_VkResult(result), (int) result);
                return std::unexpected(RhiError::Failed);
            }

            if (((props[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) && (presentSupport != 0u)) {
                physicalDevice = physicalDevices[i];
                queueFamilyIndex = j;
                queueTimestampValidBits = props[j].timestampValidBits;
                break;
            }
        }

        if (physicalDevice != VK_NULL_HANDLE) {
            break;
        }
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queueFamilyIndex,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };

    std::vector<const char*> deviceExtensions = {
        "VK_KHR_swapchain",
#ifdef __APPLE__
        "VK_KHR_portability_subset",
#endif
    };

    // Optional: calibrated timestamps align GPU zones with the CPU clock for the profiler.
    bool calibratedTimestampsAvailable = false;
    {
        uint32_t availableCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &availableCount, nullptr);
        std::vector<VkExtensionProperties> available(availableCount);
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &availableCount, available.data());
        for (const auto& ext : available) {
            if (strcmp(ext.extensionName, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0) {
                calibratedTimestampsAvailable = true;
                break;
            }
        }
    }
    if (calibratedTimestampsAvailable) {
        deviceExtensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    }
    auto deviceExtensionCount = (uint32_t) deviceExtensions.size();

    VkPhysicalDeviceSynchronization2Features sync2Features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
        .synchronization2 = VK_TRUE,
    };

    VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        .pNext = &sync2Features,
        .dynamicRendering = VK_TRUE,
    };

    VkPhysicalDeviceFeatures supportedFeatures = {};
    vkGetPhysicalDeviceFeatures(physicalDevice, &supportedFeatures);
    VkPhysicalDeviceFeatures enabledFeatures = {};
    enabledFeatures.wideLines = supportedFeatures.wideLines;
    enabledFeatures.samplerAnisotropy = supportedFeatures.samplerAnisotropy;

    VkPhysicalDeviceProperties properties = {};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    VkPhysicalDeviceDriverProperties driverProperties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driverProperties};
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
    deviceLimits = {
        .minUniformBufferOffsetAlignment = properties.limits.minUniformBufferOffsetAlignment,
        .maxPushConstantSize = properties.limits.maxPushConstantsSize,
        .maxLineWidth = supportedFeatures.wideLines == VK_TRUE ? properties.limits.lineWidthRange[1] : 1.0f,
        .wideLines = supportedFeatures.wideLines == VK_TRUE,
        .samplerAnisotropy = supportedFeatures.samplerAnisotropy == VK_TRUE,
        .timestamps = queueTimestampValidBits != 0 && properties.limits.timestampPeriod > 0.0f,
        .timestampPeriodNs = properties.limits.timestampPeriod,
    };
    std::snprintf(deviceLimits.deviceName, sizeof(deviceLimits.deviceName), "%s", properties.deviceName);
    std::snprintf(deviceLimits.driverName, sizeof(deviceLimits.driverName), "%s %s", driverProperties.driverName, driverProperties.driverInfo);

    VkDeviceCreateInfo deviceCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &dynamicRenderingFeatures,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueCreateInfo,
        .enabledExtensionCount = deviceExtensionCount,
        .ppEnabledExtensionNames = deviceExtensions.data(),
        .pEnabledFeatures = &enabledFeatures,
    };

    result = vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateDevice failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(RhiError::Failed);
    }

    vkGetDeviceQueue(device, queueFamilyIndex, 0, &graphicsQueue);

    if (calibratedTimestampsAvailable) {
        getCalibratedTimestampsFn = (PFN_vkGetCalibratedTimestampsEXT) vkGetDeviceProcAddr(device, "vkGetCalibratedTimestampsEXT");
        deviceLimits.calibratedTimestamps = getCalibratedTimestampsFn != nullptr && deviceLimits.timestamps;
    }

    VkCommandPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queueFamilyIndex,
    };
    result = vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateCommandPool failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(RhiError::Failed);
    }

    return {};
}

auto RhiDeviceVulkan::destroy() -> void {
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    if (debugMessenger != VK_NULL_HANDLE) {
        auto destroyMessenger = (PFN_vkDestroyDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroyMessenger != nullptr) {
            destroyMessenger(instance, debugMessenger, nullptr);
        }
        debugMessenger = VK_NULL_HANDLE;
    }
    vkDestroyInstance(instance, nullptr);
}

auto RhiDeviceVulkan::waitIdle() -> void {
    vkDeviceWaitIdle(device);
}

auto RhiDeviceVulkan::createSwapchain(RhiExtent2D extent) -> RhiSwapchain* {
    auto* sc = new RhiSwapchainVulkan();
    if (!sc->init(physicalDevice, device, surface, queueFamilyIndex, extent)) {
        delete sc;
        return nullptr;
    }
    return sc;
}

auto RhiDeviceVulkan::createBuffer(const RhiBufferDesc& desc) -> RhiBuffer* {
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = desc.size,
        .usage = toVkBufferUsage(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    auto* buf = new RhiBufferVulkan();
    auto result = vkCreateBuffer(device, &bufferInfo, nullptr, &buf->buffer);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateBuffer failed: {}({})", string_VkResult(result), (int) result);
        delete buf;
        return nullptr;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, buf->buffer, &memReqs);

    auto memTypeIndex = findMemoryType(memReqs.memoryTypeBits, toVkMemoryProps(desc.memory));
    if (memTypeIndex == UINT32_MAX) {
        vkDestroyBuffer(device, buf->buffer, nullptr);
        delete buf;
        return nullptr;
    }

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = memTypeIndex,
    };
    result = vkAllocateMemory(device, &allocInfo, nullptr, &buf->memory);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkAllocateMemory failed: {}({})", string_VkResult(result), (int) result);
        vkDestroyBuffer(device, buf->buffer, nullptr);
        delete buf;
        return nullptr;
    }

    result = vkBindBufferMemory(device, buf->buffer, buf->memory, 0);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkBindBufferMemory failed: {}({})", string_VkResult(result), (int) result);
        vkFreeMemory(device, buf->memory, nullptr);
        vkDestroyBuffer(device, buf->buffer, nullptr);
        delete buf;
        return nullptr;
    }

    return buf;
}

static auto formatAspect(RhiFormat format) -> VkImageAspectFlags {
    switch (format) {
        case RhiFormat::D32_SFLOAT:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case RhiFormat::D24_UNORM_S8_UINT:
        case RhiFormat::D32_SFLOAT_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

static auto toVkSampleCount(uint32_t samples) -> VkSampleCountFlagBits {
    switch (samples) {
        case 2:
            return VK_SAMPLE_COUNT_2_BIT;
        case 4:
            return VK_SAMPLE_COUNT_4_BIT;
        case 8:
            return VK_SAMPLE_COUNT_8_BIT;
        default:
            return VK_SAMPLE_COUNT_1_BIT;
    }
}

static auto toVkViewType(RhiTextureDimension dimension) -> VkImageViewType {
    switch (dimension) {
        case RhiTextureDimension::Texture2D:
            return VK_IMAGE_VIEW_TYPE_2D;
        case RhiTextureDimension::Texture2DArray:
            return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case RhiTextureDimension::TextureCube:
            return VK_IMAGE_VIEW_TYPE_CUBE;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}

auto RhiDeviceVulkan::supportsTextureFormat(RhiFormat format, RhiTextureUsageFlags usage) const -> bool {
    VkFormatProperties props = {};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, toVkFormat(format), &props);
    auto features = props.optimalTilingFeatures;

    VkFormatFeatureFlags required = 0;
    if (usage.has(RhiTextureUsage::Sampled)) {
        required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    }
    if (usage.has(RhiTextureUsage::ColorAttachment)) {
        required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    }
    if (usage.has(RhiTextureUsage::DepthAttachment)) {
        required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (usage.has(RhiTextureUsage::Storage)) {
        required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    }
    if (usage.has(RhiTextureUsage::TransferSrc)) {
        required |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    }
    if (usage.has(RhiTextureUsage::TransferDst)) {
        required |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    }
    return (features & required) == required;
}

auto RhiDeviceVulkan::createTexture(const RhiTextureDesc& desc) -> RhiTexture* {
    auto* tex = new RhiTextureVulkan();
    tex->aspect = formatAspect(desc.format);
    tex->mipLevels = desc.mipLevels;
    tex->arrayLayers = desc.arrayLayers;

    VkImageCreateFlags createFlags = 0;
    if (desc.dimension == RhiTextureDimension::TextureCube) {
        createFlags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }

    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = createFlags,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = toVkFormat(desc.format),
        .extent = {desc.width, desc.height, 1},
        .mipLevels = desc.mipLevels,
        .arrayLayers = desc.arrayLayers,
        .samples = toVkSampleCount(desc.sampleCount),
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = toVkImageUsage(desc.usage),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    auto result = vkCreateImage(device, &imageInfo, nullptr, &tex->image);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateImage failed: {}({})", string_VkResult(result), (int) result);
        delete tex;
        return nullptr;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, tex->image, &memReqs);
    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memReqs.size,
        .memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    vkAllocateMemory(device, &allocInfo, nullptr, &tex->memory);
    vkBindImageMemory(device, tex->image, tex->memory, 0);

    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = tex->image,
        .viewType = toVkViewType(desc.dimension),
        .format = toVkFormat(desc.format),
        .subresourceRange =
            {
                .aspectMask = tex->aspect,
                .baseMipLevel = 0,
                .levelCount = desc.mipLevels,
                .baseArrayLayer = 0,
                .layerCount = desc.arrayLayers,
            },
    };
    vkCreateImageView(device, &viewInfo, nullptr, &tex->view);

    return tex;
}

static auto toVkFilter(RhiFilter filter) -> VkFilter {
    return filter == RhiFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

static auto toVkMipmapMode(RhiMipmapMode mode) -> VkSamplerMipmapMode {
    return mode == RhiMipmapMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

static auto toVkAddressMode(RhiAddressMode mode) -> VkSamplerAddressMode {
    switch (mode) {
        case RhiAddressMode::Repeat:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case RhiAddressMode::MirroredRepeat:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case RhiAddressMode::ClampToEdge:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case RhiAddressMode::ClampToBorder:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

auto RhiDeviceVulkan::createSampler(const RhiSamplerDesc& desc) -> RhiSampler* {
    auto* sampler = new RhiSamplerVulkan();

    auto anisotropy = desc.maxAnisotropy;
    if (!deviceLimits.samplerAnisotropy) {
        anisotropy = 0.0f;
    }

    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = toVkFilter(desc.magFilter),
        .minFilter = toVkFilter(desc.minFilter),
        .mipmapMode = toVkMipmapMode(desc.mipmapMode),
        .addressModeU = toVkAddressMode(desc.addressU),
        .addressModeV = toVkAddressMode(desc.addressV),
        .addressModeW = toVkAddressMode(desc.addressW),
        .anisotropyEnable = anisotropy > 0.0f ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = anisotropy > 0.0f ? anisotropy : 1.0f,
        .mipLodBias = desc.mipLodBias,
        .compareEnable = desc.compareEnable ? VK_TRUE : VK_FALSE,
        .compareOp = toVkCompareOp(desc.compareOp),
        .minLod = desc.minLod,
        .maxLod = desc.maxLod,
    };
    auto result = vkCreateSampler(device, &samplerInfo, nullptr, &sampler->sampler);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateSampler failed: {}({})", string_VkResult(result), (int) result);
        delete sampler;
        return nullptr;
    }
    return sampler;
}

auto RhiDeviceVulkan::createShaderModule(const RhiShaderDesc& desc) -> RhiShaderModule* {
    if (desc.code.empty() || (desc.code.size() % 4) != 0) {
        std::println(stderr, "createShaderModule: SPIR-V code size {} is not a multiple of 4", desc.code.size());
        return nullptr;
    }

    VkShaderModuleCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = desc.code.size(),
        .pCode = (const uint32_t*) desc.code.data(),
    };

    auto* sm = new RhiShaderModuleVulkan();
    sm->entryPoint = (desc.entryPoint != nullptr) ? desc.entryPoint : "main";
    auto result = vkCreateShaderModule(device, &createInfo, nullptr, &sm->module);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateShaderModule failed: {}({})", string_VkResult(result), (int) result);
        delete sm;
        return nullptr;
    }

    return sm;
}

auto RhiDeviceVulkan::createPipelineLayout(std::span<RhiDescriptorSetLayout* const> setLayouts, const RhiPushConstantRange& pushConstant) -> VkPipelineLayout {
    std::vector<VkDescriptorSetLayout> vkSetLayouts;
    vkSetLayouts.reserve(setLayouts.size());
    for (auto* layout : setLayouts) {
        vkSetLayouts.push_back(static_cast<RhiDescriptorSetLayoutVulkan*>(layout)->layout);
    }

    VkPushConstantRange pushConstRange = {
        .stageFlags = toVkShaderStage(pushConstant.stage),
        .offset = pushConstant.offset,
        .size = pushConstant.size,
    };

    VkPipelineLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = (uint32_t) vkSetLayouts.size(),
        .pSetLayouts = vkSetLayouts.data(),
        .pushConstantRangeCount = pushConstant.size > 0 ? 1u : 0u,
        .pPushConstantRanges = pushConstant.size > 0 ? &pushConstRange : nullptr,
    };

    VkPipelineLayout layout = VK_NULL_HANDLE;
    auto result = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreatePipelineLayout failed: {}({})", string_VkResult(result), (int) result);
        return VK_NULL_HANDLE;
    }
    return layout;
}

auto RhiDeviceVulkan::createComputePipeline(const RhiComputePipelineDesc& desc) -> RhiPipeline* {
    auto* shader = static_cast<RhiShaderModuleVulkan*>(desc.shader);

    auto* pip = new RhiPipelineVulkan();
    pip->bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    pip->layout = createPipelineLayout(desc.descriptorSetLayouts, desc.pushConstant);
    if (pip->layout == VK_NULL_HANDLE) {
        delete pip;
        return nullptr;
    }

    VkComputePipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = shader->module,
                .pName = shader->entryPoint.c_str(),
            },
        .layout = pip->layout,
    };

    auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pip->pipeline);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateComputePipelines failed: {}({})", string_VkResult(result), (int) result);
        vkDestroyPipelineLayout(device, pip->layout, nullptr);
        delete pip;
        return nullptr;
    }
    return pip;
}

auto RhiDeviceVulkan::createGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) -> RhiPipeline* {
    auto* vertMod = static_cast<RhiShaderModuleVulkan*>(desc.vertexShader);
    auto* fragMod = static_cast<RhiShaderModuleVulkan*>(desc.fragmentShader);

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertMod->module,
            .pName = vertMod->entryPoint.c_str(),
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragMod->module,
            .pName = fragMod->entryPoint.c_str(),
        }};

    VkVertexInputBindingDescription bindingDesc = {
        .binding = 0,
        .stride = desc.vertexStride,
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };

    auto attrCount = (uint32_t) desc.vertexAttributes.size();
    std::vector<VkVertexInputAttributeDescription> attrDescs(attrCount);
    for (uint32_t i = 0; i < attrCount; i++) {
        attrDescs[i] = {
            .location = desc.vertexAttributes[i].location,
            .binding = desc.vertexAttributes[i].binding,
            .format = toVkFormat(desc.vertexAttributes[i].format),
            .offset = desc.vertexAttributes[i].offset,
        };
    }

    VkPipelineVertexInputStateCreateInfo vertexInputState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = attrCount > 0 ? 1u : 0u,
        .pVertexBindingDescriptions = attrCount > 0 ? &bindingDesc : nullptr,
        .vertexAttributeDescriptionCount = attrCount,
        .pVertexAttributeDescriptions = attrDescs.data(),
    };

    auto vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (desc.topology == RhiPrimitiveTopology::LineList) {
        vkTopology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = vkTopology,
    };

    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = (uint32_t) dynamicStates.size(),
        .pDynamicStates = dynamicStates.data(),
    };

    VkPipelineRasterizationStateCreateInfo rasterizationState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = toVkCullMode(desc.raster.cullMode),
        .frontFace = toVkFrontFace(desc.raster.frontFace),
        .lineWidth = desc.raster.lineWidth,
    };

    VkPipelineMultisampleStateCreateInfo multisampleState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depthStencilState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc.depth.testEnable ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = desc.depth.writeEnable ? VK_TRUE : VK_FALSE,
        .depthCompareOp = toVkCompareOp(desc.depth.compareOp),
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE,
    };

    auto colorAttachmentCount = (uint32_t) desc.colorFormats.size();
    VkPipelineColorBlendAttachmentState blendAttachment = {
        .blendEnable = desc.blend.enable ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = toVkBlendFactor(desc.blend.srcColor),
        .dstColorBlendFactor = toVkBlendFactor(desc.blend.dstColor),
        .colorBlendOp = toVkBlendOp(desc.blend.colorOp),
        .srcAlphaBlendFactor = toVkBlendFactor(desc.blend.srcAlpha),
        .dstAlphaBlendFactor = toVkBlendFactor(desc.blend.dstAlpha),
        .alphaBlendOp = toVkBlendOp(desc.blend.alphaOp),
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments(colorAttachmentCount, blendAttachment);

    VkPipelineColorBlendStateCreateInfo colorBlendState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = colorAttachmentCount,
        .pAttachments = colorBlendAttachments.data(),
    };

    auto* pip = new RhiPipelineVulkan();
    pip->bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    pip->topology = desc.topology;
    pip->layout = createPipelineLayout(desc.descriptorSetLayouts, desc.pushConstant);
    if (pip->layout == VK_NULL_HANDLE) {
        delete pip;
        return nullptr;
    }

    std::vector<VkFormat> vkColorFormats;
    vkColorFormats.reserve(desc.colorFormats.size());
    for (auto f : desc.colorFormats) {
        vkColorFormats.push_back(toVkFormat(f));
    }

    VkPipelineRenderingCreateInfo renderingInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = (uint32_t) vkColorFormats.size(),
        .pColorAttachmentFormats = vkColorFormats.data(),
        .depthAttachmentFormat = desc.depthFormat != RhiFormat::Undefined ? toVkFormat(desc.depthFormat) : VK_FORMAT_UNDEFINED,
    };

    VkGraphicsPipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &renderingInfo,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInputState,
        .pInputAssemblyState = &inputAssemblyState,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizationState,
        .pMultisampleState = &multisampleState,
        .pDepthStencilState = &depthStencilState,
        .pColorBlendState = &colorBlendState,
        .pDynamicState = &dynamicState,
        .layout = pip->layout,
    };

    auto result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pip->pipeline);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateGraphicsPipelines failed: {}({})", string_VkResult(result), (int) result);
        vkDestroyPipelineLayout(device, pip->layout, nullptr);
        delete pip;
        return nullptr;
    }

    return pip;
}

auto RhiDeviceVulkan::createDescriptorSetLayout(std::span<const RhiDescriptorBinding> bindings) -> RhiDescriptorSetLayout* {
    auto count = (uint32_t) bindings.size();
    std::vector<VkDescriptorSetLayoutBinding> vkBindings(count);
    for (uint32_t i = 0; i < count; i++) {
        auto type = toVkDescriptorType(bindings[i].type);

        vkBindings[i] = {
            .binding = bindings[i].binding,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = toVkShaderStage(bindings[i].stage),
        };
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = count,
        .pBindings = vkBindings.data(),
    };

    auto* layout = new RhiDescriptorSetLayoutVulkan();
    auto result = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &layout->layout);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateDescriptorSetLayout failed: {}({})", string_VkResult(result), (int) result);
        delete layout;
        return nullptr;
    }
    return layout;
}

auto RhiDeviceVulkan::createDescriptorPool(uint32_t maxSets, std::span<const RhiDescriptorBinding> bindings) -> RhiDescriptorPool* {
    auto bindingCount = (uint32_t) bindings.size();
    std::vector<VkDescriptorPoolSize> poolSizes(bindingCount);
    for (uint32_t i = 0; i < bindingCount; i++) {
        auto type = toVkDescriptorType(bindings[i].type);
        poolSizes[i] = {.type = type, .descriptorCount = maxSets};
    }

    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, // freeDescriptorSets returns sets individually
        .maxSets = maxSets,
        .poolSizeCount = bindingCount,
        .pPoolSizes = poolSizes.data(),
    };

    auto* pool = new RhiDescriptorPoolVulkan();
    vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool->pool);
    return pool;
}

auto RhiDeviceVulkan::allocateDescriptorSets(RhiDescriptorPool* pool, RhiDescriptorSetLayout* layout, std::span<RhiDescriptorSet*> outSets) -> bool {
    auto* vkPool = static_cast<RhiDescriptorPoolVulkan*>(pool);
    auto* vkLayout = static_cast<RhiDescriptorSetLayoutVulkan*>(layout);
    auto count = (uint32_t) outSets.size();

    std::vector<VkDescriptorSetLayout> layouts(count, vkLayout->layout);
    std::vector<VkDescriptorSet> vkSets(count);

    VkDescriptorSetAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = vkPool->pool,
        .descriptorSetCount = count,
        .pSetLayouts = layouts.data(),
    };
    auto result = vkAllocateDescriptorSets(device, &allocInfo, vkSets.data());
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkAllocateDescriptorSets failed: {}({})", string_VkResult(result), (int) result);
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        auto* ds = new RhiDescriptorSetVulkan();
        ds->set = vkSets[i];
        outSets[i] = ds;
    }
    return true;
}

auto RhiDeviceVulkan::freeDescriptorSets(RhiDescriptorPool* pool, std::span<RhiDescriptorSet* const> sets) -> void {
    auto* vkPool = static_cast<RhiDescriptorPoolVulkan*>(pool);
    std::vector<VkDescriptorSet> vkSets;
    vkSets.reserve(sets.size());
    for (auto* set : sets) {
        if (set == nullptr) {
            continue;
        }
        vkSets.push_back(static_cast<RhiDescriptorSetVulkan*>(set)->set);
    }
    if (!vkSets.empty()) {
        vkFreeDescriptorSets(device, vkPool->pool, (uint32_t) vkSets.size(), vkSets.data());
    }
    for (auto* set : sets) {
        delete set;
    }
}

auto RhiDeviceVulkan::updateDescriptorSet(RhiDescriptorSet* set, std::span<const RhiDescriptorWrite> writes) -> void {
    auto* vkSet = static_cast<RhiDescriptorSetVulkan*>(set);
    auto writeCount = (uint32_t) writes.size();

    std::vector<VkWriteDescriptorSet> vkWrites(writeCount);
    std::vector<VkDescriptorBufferInfo> bufInfos(writeCount);
    std::vector<VkDescriptorImageInfo> imgInfos(writeCount);

    for (uint32_t i = 0; i < writeCount; i++) {
        vkWrites[i] = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = vkSet->set,
            .dstBinding = writes[i].binding,
            .descriptorCount = 1,
        };

        vkWrites[i].descriptorType = toVkDescriptorType(writes[i].type);
        switch (writes[i].type) {
            case RhiDescriptorType::UniformBuffer:
            case RhiDescriptorType::StorageBuffer: {
                auto* buf = static_cast<RhiBufferVulkan*>(writes[i].buffer);
                auto range = writes[i].bufferRange > 0 ? writes[i].bufferRange : VK_WHOLE_SIZE;
                bufInfos[i] = {.buffer = buf->buffer, .offset = writes[i].bufferOffset, .range = range};
                vkWrites[i].pBufferInfo = &bufInfos[i];
                break;
            }
            case RhiDescriptorType::CombinedImageSampler: {
                auto* tex = static_cast<RhiTextureVulkan*>(writes[i].texture);
                auto* sam = static_cast<RhiSamplerVulkan*>(writes[i].sampler);
                imgInfos[i] = {
                    .sampler = sam->sampler,
                    .imageView = tex->view,
                    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                };
                vkWrites[i].pImageInfo = &imgInfos[i];
                break;
            }
            case RhiDescriptorType::StorageImage: {
                auto* tex = static_cast<RhiTextureVulkan*>(writes[i].texture);
                imgInfos[i] = {
                    .sampler = VK_NULL_HANDLE,
                    .imageView = tex->view,
                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                };
                vkWrites[i].pImageInfo = &imgInfos[i];
                break;
            }
        }
    }

    vkUpdateDescriptorSets(device, writeCount, vkWrites.data(), 0, nullptr);
}

auto RhiDeviceVulkan::createQueryPool(uint32_t timestampCount) -> RhiQueryPool* {
    VkQueryPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = timestampCount,
    };
    auto* pool = new RhiQueryPoolVulkan();
    pool->count = timestampCount;
    auto result = vkCreateQueryPool(device, &info, nullptr, &pool->pool);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateQueryPool failed: {}({})", string_VkResult(result), (int) result);
        delete pool;
        return nullptr;
    }
    return pool;
}

auto RhiDeviceVulkan::destroyQueryPool(RhiQueryPool* pool) -> void {
    auto* p = static_cast<RhiQueryPoolVulkan*>(pool);
    vkDestroyQueryPool(device, p->pool, nullptr);
    delete p;
}

auto RhiDeviceVulkan::readTimestamps(RhiQueryPool* pool, uint32_t first, std::span<uint64_t> outTicks) -> bool {
    auto* p = static_cast<RhiQueryPoolVulkan*>(pool);
    auto count = (uint32_t) outTicks.size();
    // Pairs of (value, availability); never wait, so an unwritten query cannot block the caller.
    std::vector<uint64_t> raw((size_t) count * 2);
    auto result = vkGetQueryPoolResults(
        device, p->pool, first, count, raw.size() * sizeof(uint64_t), raw.data(), 2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS && result != VK_NOT_READY) {
        std::println(stderr, "vkGetQueryPoolResults failed: {}({})", string_VkResult(result), (int) result);
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (raw[(size_t) i * 2 + 1] == 0) {
            return false;
        }
        outTicks[i] = raw[(size_t) i * 2];
    }
    return true;
}

auto RhiDeviceVulkan::calibrateGpuClock(uint64_t& gpuNs, uint64_t& cpuNs) -> bool {
    if (getCalibratedTimestampsFn == nullptr) {
        return false;
    }
    std::array<VkCalibratedTimestampInfoEXT, 2> infos = {{
        {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_DEVICE_EXT},
        {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT},
    }};
    std::array<uint64_t, 2> timestamps = {};
    std::array<uint64_t, 2> deviations = {};
    auto result = getCalibratedTimestampsFn(device, (uint32_t) infos.size(), infos.data(), timestamps.data(), deviations.data());
    if (result != VK_SUCCESS) {
        return false;
    }
    gpuNs = (uint64_t) ((double) timestamps[0] * (double) deviceLimits.timestampPeriodNs);
    cpuNs = timestamps[1]; // CLOCK_MONOTONIC is what std::chrono::steady_clock reads on Linux
    return true;
}

auto RhiDeviceVulkan::collectGpuZones(RhiCommandBuffer* cmd, std::vector<RhiGpuZone>& out) -> bool {
    auto* cb = static_cast<RhiCommandBufferVulkan*>(cmd);
    out.clear();
    if (cb->zonePool == VK_NULL_HANDLE || cb->zones.empty()) {
        return true;
    }
    auto count = (uint32_t) cb->zones.size() * 2;
    std::vector<uint64_t> raw((size_t) count * 2);
    auto result = vkGetQueryPoolResults(
        device, cb->zonePool, 0, count, raw.size() * sizeof(uint64_t), raw.data(), 2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS && result != VK_NOT_READY) {
        std::println(stderr, "vkGetQueryPoolResults failed: {}({})", string_VkResult(result), (int) result);
        return false;
    }
    auto periodNs = (double) deviceLimits.timestampPeriodNs;
    out.reserve(cb->zones.size());
    for (size_t i = 0; i < cb->zones.size(); i++) {
        if (!cb->zones[i].closed) {
            continue;
        }
        auto beginAvail = raw[(i * 2) * 2 + 1];
        auto endAvail = raw[(i * 2 + 1) * 2 + 1];
        if (beginAvail == 0 || endAvail == 0) {
            return false;
        }
        out.push_back({
            .name = cb->zones[i].name,
            .depth = cb->zones[i].depth,
            .startNs = (uint64_t) ((double) raw[(i * 2) * 2] * periodNs),
            .endNs = (uint64_t) ((double) raw[(i * 2 + 1) * 2] * periodNs),
        });
    }
    return true;
}

auto RhiDeviceVulkan::createCommandBuffer() -> RhiCommandBuffer* {
    auto* cb = new RhiCommandBufferVulkan();
    VkCommandBufferAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmdPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    auto result = vkAllocateCommandBuffers(device, &allocInfo, &cb->cmd);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkAllocateCommandBuffers failed: {}({})", string_VkResult(result), (int) result);
        delete cb;
        return nullptr;
    }
    cb->beginLabelFn = cmdBeginLabelFn;
    cb->endLabelFn = cmdEndLabelFn;

    if (deviceLimits.timestamps) {
        VkQueryPoolCreateInfo poolInfo = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = RhiCommandBufferVulkan::maxGpuZones * 2,
        };
        if (vkCreateQueryPool(device, &poolInfo, nullptr, &cb->zonePool) != VK_SUCCESS) {
            cb->zonePool = VK_NULL_HANDLE;
        }
    }
    return cb;
}

auto RhiDeviceVulkan::createSemaphore() -> RhiSemaphore* {
    auto* sem = new RhiSemaphoreVulkan();
    VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    auto result = vkCreateSemaphore(device, &info, nullptr, &sem->semaphore);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateSemaphore failed: {}({})", string_VkResult(result), (int) result);
        delete sem;
        return nullptr;
    }
    return sem;
}

auto RhiDeviceVulkan::createFence(bool signaled) -> RhiFence* {
    auto* fence = new RhiFenceVulkan();
    VkFenceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0u,
    };
    auto result = vkCreateFence(device, &info, nullptr, &fence->fence);
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkCreateFence failed: {}({})", string_VkResult(result), (int) result);
        delete fence;
        return nullptr;
    }
    return fence;
}

auto RhiDeviceVulkan::waitForFence(RhiFence* fence) -> void {
    auto* f = static_cast<RhiFenceVulkan*>(fence);
    vkWaitForFences(device, 1, &f->fence, VK_TRUE, UINT64_MAX);
}

auto RhiDeviceVulkan::resetFence(RhiFence* fence) -> void {
    auto* f = static_cast<RhiFenceVulkan*>(fence);
    vkResetFences(device, 1, &f->fence);
}

auto RhiDeviceVulkan::submitCommandBuffer(RhiCommandBuffer* cmd, const RhiSubmitInfo& info) -> void {
    auto* cb = static_cast<RhiCommandBufferVulkan*>(cmd);
    auto* waitSem = static_cast<RhiSemaphoreVulkan*>(info.waitSemaphore);
    auto* signalSem = static_cast<RhiSemaphoreVulkan*>(info.signalSemaphore);
    auto* fence = static_cast<RhiFenceVulkan*>(info.fence);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = (waitSem != nullptr) ? 1u : 0u,
        .pWaitSemaphores = (waitSem != nullptr) ? &waitSem->semaphore : nullptr,
        .pWaitDstStageMask = (waitSem != nullptr) ? &waitStage : nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb->cmd,
        .signalSemaphoreCount = (signalSem != nullptr) ? 1u : 0u,
        .pSignalSemaphores = (signalSem != nullptr) ? &signalSem->semaphore : nullptr,
    };

    vkQueueSubmit(graphicsQueue, 1, &submit, (fence != nullptr) ? fence->fence : VK_NULL_HANDLE);
}

auto RhiDeviceVulkan::present(RhiSwapchain* swapchain, RhiSemaphore* waitSemaphore, uint32_t imageIndex) -> std::expected<void, RhiError> {
    auto* sc = static_cast<RhiSwapchainVulkan*>(swapchain);
    auto* sem = static_cast<RhiSemaphoreVulkan*>(waitSemaphore);

    VkPresentInfoKHR presentInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = (sem != nullptr) ? 1u : 0u,
        .pWaitSemaphores = (sem != nullptr) ? &sem->semaphore : nullptr,
        .swapchainCount = 1,
        .pSwapchains = &sc->swapchain,
        .pImageIndices = &imageIndex,
    };

    auto result = vkQueuePresentKHR(graphicsQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return std::unexpected(toRhiError(result));
    }
    if (result != VK_SUCCESS) {
        std::println(stderr, "vkQueuePresentKHR failed: {}({})", string_VkResult(result), (int) result);
        return std::unexpected(toRhiError(result));
    }
    return {};
}

auto RhiDeviceVulkan::mapBuffer(RhiBuffer* buffer) -> void* {
    auto* buf = static_cast<RhiBufferVulkan*>(buffer);
    void* data = nullptr;
    vkMapMemory(device, buf->memory, 0, VK_WHOLE_SIZE, 0, &data);
    return data;
}

auto RhiDeviceVulkan::unmapBuffer(RhiBuffer* buffer) -> void {
    auto* buf = static_cast<RhiBufferVulkan*>(buffer);
    vkUnmapMemory(device, buf->memory);
}

auto RhiDeviceVulkan::destroyBuffer(RhiBuffer* buffer) -> void {
    auto* b = static_cast<RhiBufferVulkan*>(buffer);
    vkDestroyBuffer(device, b->buffer, nullptr);
    vkFreeMemory(device, b->memory, nullptr);
    delete b;
}

auto RhiDeviceVulkan::destroyTexture(RhiTexture* texture) -> void {
    auto* t = static_cast<RhiTextureVulkan*>(texture);
    vkDestroyImageView(device, t->view, nullptr);
    vkDestroyImage(device, t->image, nullptr);
    vkFreeMemory(device, t->memory, nullptr);
    delete t;
}

auto RhiDeviceVulkan::destroySampler(RhiSampler* sampler) -> void {
    auto* s = static_cast<RhiSamplerVulkan*>(sampler);
    vkDestroySampler(device, s->sampler, nullptr);
    delete s;
}

auto RhiDeviceVulkan::destroyShaderModule(RhiShaderModule* module) -> void {
    auto* m = static_cast<RhiShaderModuleVulkan*>(module);
    vkDestroyShaderModule(device, m->module, nullptr);
    delete m;
}

auto RhiDeviceVulkan::destroyPipeline(RhiPipeline* pipeline) -> void {
    auto* p = static_cast<RhiPipelineVulkan*>(pipeline);
    vkDestroyPipeline(device, p->pipeline, nullptr);
    vkDestroyPipelineLayout(device, p->layout, nullptr);
    delete p;
}

auto RhiDeviceVulkan::destroyDescriptorSetLayout(RhiDescriptorSetLayout* layout) -> void {
    auto* l = static_cast<RhiDescriptorSetLayoutVulkan*>(layout);
    vkDestroyDescriptorSetLayout(device, l->layout, nullptr);
    delete l;
}

auto RhiDeviceVulkan::destroyDescriptorPool(RhiDescriptorPool* pool) -> void {
    auto* p = static_cast<RhiDescriptorPoolVulkan*>(pool);
    vkDestroyDescriptorPool(device, p->pool, nullptr);
    delete p;
}

auto RhiDeviceVulkan::destroySemaphore(RhiSemaphore* semaphore) -> void {
    auto* s = static_cast<RhiSemaphoreVulkan*>(semaphore);
    vkDestroySemaphore(device, s->semaphore, nullptr);
    delete s;
}

auto RhiDeviceVulkan::destroyFence(RhiFence* fence) -> void {
    auto* f = static_cast<RhiFenceVulkan*>(fence);
    vkDestroyFence(device, f->fence, nullptr);
    delete f;
}

auto RhiDeviceVulkan::destroyCommandBuffer(RhiCommandBuffer* cmd) -> void {
    auto* cb = static_cast<RhiCommandBufferVulkan*>(cmd);
    if (cb->zonePool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device, cb->zonePool, nullptr);
    }
    vkFreeCommandBuffers(device, cmdPool, 1, &cb->cmd);
    delete cb;
}
