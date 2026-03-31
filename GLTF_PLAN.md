# GLTF Model Support — Incremental Learning Plan

Each step introduces one new Vulkan concept and produces a visible result. Do one step at a time.

## Step 1: Vertex Buffers
Move triangle vertex data from the shader to a GPU buffer.
- Define `Vertex` struct (vec2 position, vec3 color)
- Create VkBuffer + VkDeviceMemory (HOST_VISIBLE | HOST_COHERENT)
- Update vertex shader to accept `in` attributes instead of `gl_VertexIndex`
- Update VkPipelineVertexInputStateCreateInfo with bindings/attributes
- Bind vertex buffer in command recording

## Step 2: Index Buffers
Use indexed drawing to share vertices.
- Create index buffer (VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
- Draw a quad with 4 vertices + 6 indices
- Switch from vkCmdDraw to vkCmdDrawIndexed

## Step 3: Staging Buffers
Use GPU-local memory via staging transfers.
- Staging buffer (HOST_VISIBLE) → device-local buffer via vkCmdCopyBuffer
- Helper for single-time command buffers
- Apply to both vertex and index buffers

## Step 4: Uniform Buffers & MVP Transforms
Add model-view-projection matrices via descriptor sets.
- Small math library (mat4: identity, perspective, lookAt, multiply, rotate)
- UniformBufferObject: model, view, proj matrices
- Descriptor set layout, pool, sets
- Per-frame uniform buffer updates, dynamic command recording
- Rotating quad in 3D with perspective

## Step 5: Depth Buffer
Enable correct depth ordering.
- Depth image + image view (D32_SFLOAT or D24_UNORM_S8_UINT)
- Render pass depth attachment, framebuffer update
- Pipeline depth/stencil state

## Step 6: Loading a 3D Mesh
Load vertex/index data from a simple format.
- Extend Vertex with normals, add diffuse lighting in shaders
- Hardcode a cube or load simple OBJ

## Step 7: Texture Mapping
Sample textures in the fragment shader.
- stb_image.h for image loading
- Texture VkImage, staging upload, layout transitions
- VkSampler, combined image sampler descriptor
- Extend Vertex with texCoord

## Step 8: GLTF Loading
Parse and render GLTF models.
- cgltf (single-header parser)
- Extract positions, normals, texcoords, indices from accessors
- Load textures from GLTF materials
- Single mesh + single material initially

## Step 9: Scene Graph & Multiple Meshes
Render full GLTF scenes.
- Walk node tree with accumulated transforms
- Push constants or dynamic UBO for per-object model matrices
- Multiple materials/textures
