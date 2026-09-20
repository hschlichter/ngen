# RHI compute

**Status. Landed.**

## Current state

The RHI (`src/rhi/README.md`) had graphics only: `createGraphicsPipeline`, `draw`, `drawIndexed`, uniform and combined image sampler
descriptors, layout-only image barriers. Listed as the first gap likely to matter. The engine has no compute pass yet; the first consumer is the
`compute` example, which also serves as the verification.

## Scope

**In**

- `RhiShaderStage::Compute`, `RhiBufferUsage::Storage`, `RhiDescriptorType::StorageBuffer` and `StorageImage`.
- `RhiComputePipelineDesc { shader, descriptorSetLayouts, pushConstant }` and `RhiDevice::createComputePipeline`. Same `RhiPipeline` type as
  graphics; the backend records the bind point and `bindPipeline` / `bindDescriptorSet` / `pushConstants` pick it from the pipeline.
- `RhiCommandBuffer::dispatch(x, y, z)`.
- `RhiTextureState::General` for storage images. `RhiBufferState` and `RhiBufferBarrierDesc`; `pipelineBarrier(images, buffers)` with the
  old single-span call kept as a forwarding overload and `bufferBarrier(buffers)` added.
- `ShaderReadOnly` now covers vertex, fragment and compute stages instead of fragment only.
- `src/rhi/examples/compute.cpp`: storage image written by one dispatch and sampled by a quad; storage buffer written by another dispatch and
  bound as a vertex buffer. Both barrier kinds every frame.

**Out**

- Async compute queue, indirect dispatch, buffer views / texel buffers, subgroup or feature queries, renderer compute passes.

## Decisions

- **One pipeline type.** Rejected a separate `RhiComputePipeline`: the validation layer already catches a graphics pipeline bound at a dispatch,
  and every command taking a pipeline would have needed a second overload.
- **Buffer states, not masks.** `RhiBufferState { Undefined, VertexRead, IndexRead, UniformRead, StorageRead, StorageWrite, TransferSrc,
  TransferDst }` mirrors `RhiTextureState` and maps to D3D12 resource states directly. Backend derives Vulkan stage and access masks; shader
  states use vertex, fragment and compute stages together, which is conservative but correct.
- **Storage image layout is derived from the descriptor type.** `RhiDescriptorWrite` gained no layout field: `StorageImage` writes use
  `General`, `CombinedImageSampler` writes use `ShaderReadOnly`.

## Steps

All landed in one change: interface in `rhitypes.h`, `rhidevice.h`, `rhicommandbuffer.h`; Vulkan in `rhidevicevulkan.*` (pipeline layout
creation extracted into `createPipelineLayout`, shared by both pipeline kinds), `rhicommandbuffervulkan.*`; example target `ngen-example-compute`.

## Verification

- `SDL_VIDEODRIVER=offscreen ./_out/linux-vulkan/debug/ngen-example-compute --frames=60 --check --validation` exits 0: three checkerboard
  cells sampled from the storage image match the compute shader's formula; the computed quad's centre is the colour the shader wrote; a pixel
  just outside the computed quad is the clear colour, proving the positions came from the buffer.
- Same with `--resize-at=20`.
- All ten earlier examples still pass with `--check --validation`; `ngen-view` builds and the three_cubes headless run is unchanged.

## Deferred / follow-ups

- Async compute queue: trigger, a renderer pass that overlaps compute with graphics.
- Indirect dispatch and draw: trigger, GPU-driven culling.
- Renderer compute pass in the frame graph: separate plan when the first use appears.
