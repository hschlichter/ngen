#include "depthprepass.h"

#include "mesh.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "shaderloader.h"

#include <array>

auto DepthPrepass::init(RhiDevice* device, RhiFormat depthFormat, RhiDescriptorSetLayout* geometrySetLayout) -> bool {
    using enum RhiFormat;

    vertShader = loadShaderModule(device, RhiShaderStage::Vertex, "shaders/depthonly.vert.spv");
    fragShader = loadShaderModule(device, RhiShaderStage::Fragment, "shaders/shadow.frag.spv");

    std::array<RhiVertexAttribute, 1> vertexAttrs = {{
        {.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = 0}, // position-only stream
    }};

    RhiGraphicsPipelineDesc pipelineDesc = {
        .vertexShader = vertShader,
        .fragmentShader = fragShader,
        .descriptorSetLayouts = {&geometrySetLayout, 1},
        .pushConstant = {}, // model matrix comes from the instance buffer at gl_InstanceIndex
        .colorFormats = {},
        .depthFormat = depthFormat,
        .vertexStride = sizeof(std::array<float, 3>),
        .vertexAttributes = vertexAttrs,
        .raster = {.cullMode = RhiCullMode::Back},
    };
    pipelineCullBack = device->createGraphicsPipeline(pipelineDesc);
    pipelineDesc.raster.cullMode = RhiCullMode::None;
    pipelineCullNone = device->createGraphicsPipeline(pipelineDesc);
    return pipelineCullBack != nullptr && pipelineCullNone != nullptr;
}

auto DepthPrepass::destroy(RhiDevice* device) -> void {
    device->destroyPipeline(pipelineCullBack);
    device->destroyPipeline(pipelineCullNone);
    device->destroyShaderModule(vertShader);
    device->destroyShaderModule(fragShader);
}

auto DepthPrepass::addPass(
    FrameGraph& fg,
    FgTextureHandle depthHandle,
    RhiExtent2D extent,
    uint32_t imageIndex,
    std::span<const GpuInstance> instances,
    FgBufferHandle instanceBuffer,
    std::span<const uint8_t> visible,
    const GpuScene& scene,
    std::span<RhiDescriptorSet*> descriptorSets) -> const DepthPrepassData& {
    auto* cullBack = pipelineCullBack;
    auto* cullNone = pipelineCullNone;

    return fg.addPass<DepthPrepassData>(
        "DepthPrepass",
        [&](FrameGraphBuilder& builder, DepthPrepassData& data) {
            data.depth = builder.write(depthHandle, FgAccessFlags::DepthAttachment);
            builder.read(instanceBuffer, FgAccessFlags::StorageRead);
            builder.setSideEffects(true);
        },
        [cullBack, cullNone, extent, imageIndex, instances, visible, &scene, descriptorSets](FrameGraphContext& ctx, const DepthPrepassData& data) {
            auto* cmd = ctx.cmd();

            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(data.depth),
                .state = RhiTextureState::DepthStencilAttachment,
                .clear = true,
                .clearDepth = 1.0f,
            };
            RhiRenderingInfo info = {
                .extent = extent,
                .depthAttachment = &depthAtt,
            };
            cmd->beginRendering(info);
            cmd->setViewport(extent);
            cmd->setScissor(extent);

            bool useVisible = visible.size() == instances.size();
            // Single-sided instances first under back-face culling, then the double-sided
            // ones without it: one pipeline bind per group.
            for (bool doubleSided : {false, true}) {
                auto* pip = doubleSided ? cullNone : cullBack;
                bool bound = false;
                for (uint32_t m = 0; m < (uint32_t) instances.size(); m++) {
                    const auto& inst = instances[m];
                    if (inst.doubleSided != doubleSided) {
                        continue;
                    }
                    if (useVisible && visible[m] == 0) {
                        continue;
                    }
                    const auto* range = scene.meshRange(inst.mesh.index);
                    if (range == nullptr) {
                        continue;
                    }
                    if (!bound) {
                        cmd->bindPipeline(pip);
                        cmd->bindVertexBuffer(scene.positionBuffer());
                        cmd->bindIndexBuffer(scene.indexBuffer(), RhiIndexType::Uint32);
                        bound = true;
                    }
                    cmd->bindDescriptorSet(pip, 0, descriptorSets[(imageIndex * (uint32_t) instances.size()) + m]);
                    ctx.beginDraw({.instance = m, .mesh = inst.mesh.index, .material = inst.material.index, .prim = inst.prim, .indexOffset = inst.indexOffset, .indexCount = inst.indexCount});
                    cmd->drawIndexed(inst.indexCount, 1, range->firstIndex + inst.indexOffset, range->vertexOffset, m);
                    ctx.endDraw();
                }
            }

            cmd->endRendering();
        });
}
