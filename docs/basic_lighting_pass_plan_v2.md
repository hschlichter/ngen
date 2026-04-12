# Basic Lighting Pass — Implementation Plan v2

## Goal

Split the current single-pass forward renderer into a two-pass deferred-style pipeline:

1. **Geometry pass** — writes albedo + world-space normals to G-buffer MRT targets (plus depth).
2. **Lighting pass** — reads G-buffer textures, evaluates one directional light + ambient, writes final color to the swapchain image.

Debug lines and editor UI continue to draw after the lighting pass, unchanged.

---

## What exists today

| Item | Details |
|---|---|
| **Forward pass** | `renderer.h:67` — `pipeline`, `vertShader` (triangle.vert), `fragShader` (triangle.frag). Writes directly to swapchain color + depth. Fragment shader computes hardcoded directional light inline (`triangle.frag:11-16`). |
| **Descriptor layout** | `renderer.cpp:78-81` — binding 0: UBO (view/proj), binding 1: CombinedImageSampler (material texture). One set per swapchain-image × instance. |
| **Push constants** | `renderer.cpp:99` — per-instance model matrix (64 bytes, vertex stage). |
| **Frame graph** | Supports `createTexture`, `importTexture`, `read`/`write` with `FgAccessFlags` including `ShaderRead` and `ColorAttachment`. Automatic barrier insertion and transient resource lifetime via `ResourcePool`. |
| **RenderLight** | `renderworld.h:21-26` — already defined (type, color, intensity, worldTransform) but unused by the renderer. |
| **Available formats** | `rhitypes.h:40-50` — `R8G8B8A8_UNORM` (albedo), `R32G32B32A32_SFLOAT` (normals, overkill but only option with alpha channel currently). |

---

## Scope

- One directional light + ambient. Light params come from `RenderWorld::lights` if populated, otherwise sensible defaults (direction `(1,1,1)`, white, intensity 1.0, ambient 0.15).
- No changes to mesh upload, material system, or descriptor set allocation for the geometry pass.
- Overlay passes (debug lines, editor UI) stay exactly as-is.

---

## File changes

### 1. `src/renderer/renderer.h`

Rename forward-pass members and add lighting-pass members.

**Rename** (comment + variable names only, same types):
```
pipeline              → geometryPipeline
descriptorSetLayout   → geometryDescriptorSetLayout
descriptorPool        → geometryDescriptorPool
descriptorSets        → geometryDescriptorSets
vertShader            → geometryVertShader
fragShader            → geometryFragShader
```

**Add** (new members):
```cpp
// Lighting pass
RhiPipeline* lightingPipeline = nullptr;
RhiDescriptorSetLayout* lightingDescriptorSetLayout = nullptr;
RhiDescriptorPool* lightingDescriptorPool = nullptr;
std::vector<RhiDescriptorSet*> lightingDescriptorSets;  // per swapchain image
RhiShaderModule* lightingVertShader = nullptr;
RhiShaderModule* lightingFragShader = nullptr;
```

The `textureSampler` (line 71) is reused for both G-buffer sampling and material sampling — no new sampler needed.

**Add** lighting UBO struct (alongside `UniformBufferObject`):
```cpp
struct LightingUBO {
    glm::vec4 lightDirection;  // xyz = direction, w = intensity
    glm::vec4 lightColor;     // xyz = color, w = ambient
};
```

**Add** per-frame lighting UBO storage:
```cpp
std::vector<RhiBuffer*> lightingUniformBuffers;
std::vector<void*> lightingUniformBuffersMapped;
```

---

### 2. `src/renderer/renderer.cpp` — `Renderer::init`

#### Geometry pipeline (replaces forward pipeline, ~line 74-102)

- Load `shaders/gbuffer.vert.spv` and `shaders/gbuffer.frag.spv` instead of `triangle.vert/frag`.
- Same descriptor layout (UBO + texture sampler), same vertex attributes, same push constant.
- **Key change**: two color attachment formats instead of one:
  ```cpp
  std::array<RhiFormat, 2> geoColorFormats = {
      RhiFormat::R8G8B8A8_UNORM,       // RT0: albedo
      RhiFormat::R32G32B32A32_SFLOAT,   // RT1: world normal
  };
  ```
- Pipeline desc uses `colorFormats = geoColorFormats`, keeps `depthFormat`, same vertex stride/attrs/push constant.

#### Lighting pipeline (new, after geometry pipeline creation)

- Load `shaders/lighting.vert.spv` and `shaders/lighting.frag.spv`.
- New descriptor layout with 3 bindings:
  ```cpp
  std::array<RhiDescriptorBinding, 3> lightBindings = {{
      {.binding = 0, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},  // albedo
      {.binding = 1, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},  // normal
      {.binding = 2, .type = UniformBuffer, .stage = RhiShaderStage::Fragment},          // LightingUBO
  }};
  ```
- Pipeline desc:
  - `colorFormats = {swapchain->colorFormat()}` (single output — swapchain image)
  - `depthFormat = RhiFormat::Undefined` (no depth)
  - `vertexStride = 0`, empty `vertexAttributes` (fullscreen triangle generated in shader)
  - `depthTestEnable = false`, `depthWriteEnable = false`
  - No push constants
- Allocate descriptor pool: `maxSets = imgCount`, bindings = `lightBindings`.
- Allocate `imgCount` descriptor sets.
- Allocate per-frame `lightingUniformBuffers` (same pattern as existing UBOs — CpuToGpu, size = `sizeof(LightingUBO)`).
- Map each lighting UBO.

**Note on descriptor set updates**: The lighting descriptor sets reference G-buffer textures which are transient frame-graph resources — they get (re)allocated each frame by the `ResourcePool`. This means the descriptor sets must be updated every frame inside the render function, not at init time. At init, only allocate the sets and UBO buffers.

#### Lighting descriptor set update (each frame, in `Renderer::render`)

After the frame graph allocates the G-buffer transient textures (but before execute), or more practically: inside the geometry pass execute callback after we know the physical textures, we update the lighting descriptor sets. However, since the frame graph allocates transients during `execute()` and we need the physical texture pointers, the cleanest approach is:

- **Option A**: Update lighting descriptors inside the lighting pass execute callback, just before the draw. The physical textures are available via `ctx.texture(handle)` at that point.
- **Option B**: Make the G-buffer textures non-transient (imported external textures that persist across frames, manually managed). Simpler descriptor management but wastes memory when window resizes.

**Go with Option A** — update descriptors in the lighting pass execute lambda. This is one `updateDescriptorSet` call per frame, which is cheap.

---

### 3. `src/renderer/renderer.cpp` — `Renderer::render`

Replace the single `ForwardPass` with `GeometryPass` + `LightingPass`. Debug and EditorUI passes stay unchanged.

#### Resource setup (after `frameGraph.reset()`)

```cpp
auto colorHandle = frameGraph.importTexture(swapchain->image(*index), {...});
auto depthHandle = frameGraph.importTexture(swapchain->depthImage(), {...});

// Transient G-buffer textures
FgTextureDesc albedoDesc = {ext.width, ext.height, RhiFormat::R8G8B8A8_UNORM,
                            RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled};
FgTextureDesc normalDesc = {ext.width, ext.height, RhiFormat::R32G32B32A32_SFLOAT,
                            RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled};
auto gbufferAlbedo = frameGraph.createTexture(albedoDesc);  // note: this is builder.createTexture inside setup
auto gbufferNormal = frameGraph.createTexture(normalDesc);
```

Wait — `createTexture` is on `FrameGraphBuilder`, only available inside a pass setup lambda. So the transient textures must be created in the geometry pass setup:

```cpp
// Inside GeometryPass setup lambda:
data.albedo = builder.createTexture(albedoDesc);
data.normal = builder.createTexture(normalDesc);
data.albedo = builder.write(data.albedo, FgAccessFlags::ColorAttachment);
data.normal = builder.write(data.normal, FgAccessFlags::ColorAttachment);
data.depth  = builder.write(depthHandle, FgAccessFlags::DepthAttachment);
```

#### GeometryPass

**Pass data struct**:
```cpp
struct GeometryPassData {
    FgTextureHandle albedo;
    FgTextureHandle normal;
    FgTextureHandle depth;
};
```

**Setup**: create transient albedo/normal, write all three (albedo, normal, depth).

**Execute**: Same draw loop as current ForwardPass, but:
- Two color attachments: albedo (clear to black) and normal (clear to `(0,0,0,0)`)
- Bind `geometryPipeline`
- Same push constants, vertex/index buffer binding, descriptor set binding as today

#### LightingPass

**Pass data struct**:
```cpp
struct LightingPassData {
    FgTextureHandle albedo;
    FgTextureHandle normal;
    FgTextureHandle color;
};
```

**Setup**:
```cpp
data.albedo = builder.read(geomData.albedo, FgAccessFlags::ShaderRead);
data.normal = builder.read(geomData.normal, FgAccessFlags::ShaderRead);
data.color  = builder.write(colorHandle, FgAccessFlags::ColorAttachment);
```

**Execute**:
```cpp
// Update lighting UBO
LightingUBO lightUbo = defaultLightParams();
if (!renderWorldLights.empty()) {
    // extract from first directional light
}
memcpy(lightingUniformBuffersMapped[imageIdx], &lightUbo, sizeof(lightUbo));

// Update descriptor set with this frame's physical G-buffer textures
std::array<RhiDescriptorWrite, 3> writes = {{
    {.binding = 0, .type = CombinedImageSampler, .texture = ctx.texture(data.albedo), .sampler = textureSampler},
    {.binding = 1, .type = CombinedImageSampler, .texture = ctx.texture(data.normal), .sampler = textureSampler},
    {.binding = 2, .type = UniformBuffer, .buffer = lightingUniformBuffers[imageIdx],
     .bufferRange = sizeof(LightingUBO)},
}};
device->updateDescriptorSet(lightingDescriptorSets[imageIdx], writes);

// Draw fullscreen triangle
RhiRenderingAttachmentInfo colorAtt = {
    .texture = ctx.texture(data.color),
    .layout = RhiImageLayout::ColorAttachment,
    .clear = true,
    .clearColor = {0, 0, 0, 1},
};
RhiRenderingInfo renderInfo = {.extent = ext, .colorAttachments = {&colorAtt, 1}};
cmd->beginRendering(renderInfo);
cmd->bindPipeline(lightingPipeline);
cmd->bindDescriptorSet(lightingPipeline, lightingDescriptorSets[imageIdx]);
cmd->draw(3, 1, 0, 0);  // fullscreen triangle, no vertex buffer
cmd->endRendering();
```

#### Pass ordering

1. GeometryPass — writes gbufferAlbedo, gbufferNormal, depth
2. LightingPass — reads gbufferAlbedo, gbufferNormal; writes swapchain color
3. DebugLinePass — writes swapchain color + depth (unchanged)
4. EditorUIPass — writes swapchain color (unchanged)

The frame graph topological sort handles ordering automatically from the read/write declarations.

---

### 4. `src/renderer/renderer.cpp` — `Renderer::destroy`

Add cleanup for new resources (before existing cleanup, reverse creation order):

```cpp
// Lighting pass
for (auto* buf : lightingUniformBuffers) device->destroyBuffer(buf);
device->destroyDescriptorPool(lightingDescriptorPool);
device->destroyDescriptorSetLayout(lightingDescriptorSetLayout);
device->destroyPipeline(lightingPipeline);
device->destroyShaderModule(lightingFragShader);
device->destroyShaderModule(lightingVertShader);
```

Rename existing forward pass cleanup to use `geometry*` names.

---

### 5. `src/renderer/renderer.cpp` — `Renderer::uploadRenderWorld`

Rename all references from `descriptorPool`/`descriptorSets`/`descriptorSetLayout` to `geometryDescriptorPool`/`geometryDescriptorSets`/`geometryDescriptorSetLayout`. No logic changes.

---

### 6. New shaders

#### `shaders/gbuffer.vert`

Identical to current `triangle.vert`. Same UBO (set 0 binding 0), same push constant (model matrix), same vertex inputs. Outputs `fragNormal`, `fragColor`, `fragTexCoord` to the fragment stage.

```glsl
#version 450

layout(set = 0, binding = 0) uniform UBO {
    mat4 view;
    mat4 proj;
} ubo;

layout(push_constant) uniform Push {
    mat4 model;
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec2 fragTexCoord;

void main() {
    gl_Position = ubo.proj * ubo.view * push.model * vec4(inPosition, 1.0);
    fragNormal = mat3(push.model) * inNormal;
    fragColor = inColor;
    fragTexCoord = inTexCoord;
}
```

#### `shaders/gbuffer.frag`

Outputs to two render targets instead of computing lighting.

```glsl
#version 450

layout(set = 0, binding = 1) uniform sampler2D texSampler;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec2 fragTexCoord;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;

void main() {
    vec4 texColor = texture(texSampler, fragTexCoord);
    outAlbedo = vec4(texColor.rgb * fragColor, 1.0);
    outNormal = vec4(normalize(fragNormal) * 0.5 + 0.5, 1.0);  // encode to [0,1]
}
```

#### `shaders/lighting.vert`

Generates a fullscreen triangle with no vertex buffer.

```glsl
#version 450

layout(location = 0) out vec2 fragTexCoord;

void main() {
    // Fullscreen triangle: vertices at (-1,-1), (3,-1), (-1,3)
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    fragTexCoord = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
```

#### `shaders/lighting.frag`

Samples G-buffer, evaluates directional light + ambient.

```glsl
#version 450

layout(set = 0, binding = 0) uniform sampler2D gbufferAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gbufferNormal;
layout(set = 0, binding = 2) uniform LightUBO {
    vec4 lightDirection;  // xyz = dir, w = intensity
    vec4 lightColor;      // xyz = color, w = ambient
} light;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 albedo = texture(gbufferAlbedo, fragTexCoord).rgb;
    vec3 normal = texture(gbufferNormal, fragTexCoord).rgb * 2.0 - 1.0;  // decode from [0,1]
    normal = normalize(normal);

    vec3 lightDir = normalize(light.lightDirection.xyz);
    float diff = max(dot(normal, lightDir), 0.0);
    float ambient = light.lightColor.w;

    vec3 lit = albedo * light.lightColor.rgb * (ambient + diff * light.lightDirection.w);
    outColor = vec4(lit, 1.0);
}
```

---

### 7. G-buffer debug visualization

Add a debug view mode so individual G-buffer channels can be inspected at runtime via the existing Debug menu.

#### `src/renderer/renderer.h`

Add an enum and members to hold the debug view state:

```cpp
enum class GBufferView : int {
    Lit = 0,    // normal lighting output
    Albedo,     // G-buffer albedo only (fullscreen)
    Normals,    // G-buffer normals only (fullscreen)
    Depth,      // linearized depth (future, once depth is readable)
};

// In Renderer class, private:
GBufferView gbufferView = GBufferView::Lit;
bool showBufferOverlay = false;  // toggle the preview strip
```

Add public accessors so the UI can read/write them:

```cpp
auto gbufferViewMode() -> GBufferView& { return gbufferView; }
auto bufferOverlayEnabled() -> bool& { return showBufferOverlay; }
```

#### `shaders/lighting.frag` — push constant for view mode + overlay

The push constant carries both the fullscreen view mode and the overlay toggle. All debug visualization is handled purely in the fragment shader by checking UV coordinates — no extra draw calls or passes.

```glsl
layout(push_constant) uniform Push {
    int viewMode;      // 0=Lit, 1=Albedo, 2=Normals
    int showOverlay;   // 0=off, 1=on
} push;
```

**Overlay layout**: a horizontal strip along the bottom of the viewport showing side-by-side preview rectangles for each buffer. Each preview is 1/4 viewport width, with a fixed height (e.g. 1/4 viewport height), anchored to the bottom-left. A thin dark border separates them.

```glsl
// Preview strip constants
const float STRIP_HEIGHT = 0.25;        // 25% of viewport height
const float PREVIEW_WIDTH = 0.25;       // each preview = 25% of viewport width
const float BORDER = 0.003;             // thin dark border between previews
const int   NUM_PREVIEWS = 3;           // albedo, normals, lit

vec3 sampleBuffer(int mode, vec2 uv) {
    vec3 albedo = texture(gbufferAlbedo, uv).rgb;
    if (mode == 1) return albedo;
    vec3 normal = texture(gbufferNormal, uv).rgb * 2.0 - 1.0;
    if (mode == 2) return normalize(normal) * 0.5 + 0.5;
    // mode 0: lit
    vec3 lightDir = normalize(light.lightDirection.xyz);
    float diff = max(dot(normalize(normal), lightDir), 0.0);
    float ambient = light.lightColor.w;
    return albedo * light.lightColor.rgb * (ambient + diff * light.lightDirection.w);
}

void main() {
    vec2 uv = fragTexCoord;

    // Check if fragment falls inside the overlay strip
    if (push.showOverlay == 1 && uv.y > (1.0 - STRIP_HEIGHT)) {
        float stripLocalY = (uv.y - (1.0 - STRIP_HEIGHT)) / STRIP_HEIGHT;
        float stripLocalX = uv.x;

        int previewIdx = int(floor(stripLocalX / PREVIEW_WIDTH));
        if (previewIdx < NUM_PREVIEWS) {
            // Local UV within this preview rectangle
            float localX = (stripLocalX - float(previewIdx) * PREVIEW_WIDTH) / PREVIEW_WIDTH;
            float localY = stripLocalY;

            // Border check
            if (localX < BORDER / PREVIEW_WIDTH || localX > 1.0 - BORDER / PREVIEW_WIDTH ||
                localY < BORDER / STRIP_HEIGHT  || localY > 1.0 - BORDER / STRIP_HEIGHT) {
                outColor = vec4(0.0, 0.0, 0.0, 1.0);
                return;
            }

            // Preview order: Albedo (1), Normals (2), Lit (0)
            int modes[3] = int[3](1, 2, 0);
            vec2 sampleUV = vec2(localX, localY);
            outColor = vec4(sampleBuffer(modes[previewIdx], sampleUV), 1.0);
            return;
        }
    }

    // Fullscreen output (respects viewMode)
    outColor = vec4(sampleBuffer(push.viewMode, uv), 1.0);
}
```

The preview strip renders at the bottom-left: `[Albedo][Normals][Lit]`, each 25% of viewport width and height. The remaining bottom-right 25% shows the normal lit scene through. The border gives visual separation. When the overlay is off, the shader skips straight to the fullscreen output — zero cost.

#### Lighting pipeline push constant

```cpp
// Push constant for lighting pipeline — 8 bytes (2 ints)
.pushConstant = {.stage = RhiShaderStage::Fragment, .offset = 0, .size = 2 * sizeof(int32_t)},
```

#### Lighting pass execute callback

```cpp
struct LightingPush {
    int32_t viewMode;
    int32_t showOverlay;
};
LightingPush push = {
    .viewMode = static_cast<int32_t>(gbufferView),
    .showOverlay = showBufferOverlay ? 1 : 0,
};
cmd->pushConstants(lightingPipeline, RhiShaderStage::Fragment, 0, sizeof(push), &push);
```

#### `src/ui/mainmenubar.h` / `mainmenubar.cpp` — menu integration

Add references to `MainMenuBarState` (same pattern as `showAABBs`):

```cpp
// mainmenubar.h — add to MainMenuBarState:
int& gbufferView;       // cast to/from GBufferView at call site
bool& showBufferOverlay;
```

In `drawMainMenuBar`, extend the existing Debug menu:

```cpp
if (ImGui::BeginMenu("Debug")) {
    ImGui::MenuItem("Show AABBs", nullptr, &state.showAABBs);
    ImGui::MenuItem("Show Selected AABB", nullptr, &state.showSelectedAABB);
    ImGui::Separator();
    ImGui::MenuItem("Show Buffer Overlay", nullptr, &state.showBufferOverlay);
    ImGui::Separator();
    ImGui::Text("Fullscreen Buffer View");
    ImGui::RadioButton("Lit",     &state.gbufferView, 0);
    ImGui::RadioButton("Albedo",  &state.gbufferView, 1);
    ImGui::RadioButton("Normals", &state.gbufferView, 2);
    ImGui::EndMenu();
}
```

#### `src/ui/editorui.h` / `editorui.cpp` — threading through

Add members to `EditorUI` (alongside existing `showAABBsFlag` etc.):

```cpp
int gbufferViewMode = 0;
bool showBufferOverlay = false;
```

Pass into `MainMenuBarState` in `EditorUI::draw`. Bridge to renderer in the main loop:

```cpp
// In main loop, after editorUI.draw():
renderer.gbufferViewMode() = static_cast<GBufferView>(editorUI.gbufferViewMode());
renderer.bufferOverlayEnabled() = editorUI.bufferOverlayEnabled();
```

---

### 8. `Makefile`

No changes needed. The wildcard `$(wildcard *.vert **/*.vert)` discovers shaders in `shaders/`. The generic `%.spv: %` rule compiles them. Verify at build time that `shaders/gbuffer.vert.spv`, `shaders/gbuffer.frag.spv`, `shaders/lighting.vert.spv`, `shaders/lighting.frag.spv` appear in the build output.

---

## Known issues to resolve during implementation

1. **Normal format**: `R32G32B32A32_SFLOAT` is 16 bytes/pixel — heavy for a normal buffer. If `R16G16B16A16_SFLOAT` or `R8G8B8A8_SNORM` are needed, add them to `RhiFormat` enum and handle the Vulkan format mapping in the backend. For v1 this is acceptable; optimize later.

2. **ResourcePool key matching**: The resource pool matches on `{width, height, format, usage}`. Confirm that the `FgTextureDesc.usage` field flows through correctly so transient G-buffer textures get `ColorAttachment | Sampled` usage flags (needed for both writing as RT and reading as sampled texture).

3. **Descriptor set update per frame**: Calling `updateDescriptorSet` every frame inside the lighting pass execute callback works but assumes the Vulkan backend doesn't require the previous frame's descriptor set to be idle. Since we wait on the in-flight fence at the top of `render()`, the previous use of that descriptor set is guaranteed complete — this is safe.

4. **Pipeline creation with zero vertex stride**: The Vulkan backend's `createGraphicsPipeline` needs to handle the case where `vertexStride = 0` and `vertexAttributes` is empty (no vertex input state). Verify the Vulkan pipeline creation code doesn't assert on empty vertex input. If it does, add a conditional skip for the vertex input binding description.

---

## Implementation order

### Step 1 — Rename only (no behavior change)
Rename all forward-pass members to `geometry*` names throughout `renderer.h` and `renderer.cpp`. Build and run to confirm nothing breaks.

### Step 2 — G-buffer shaders + geometry pipeline
- Add `gbuffer.vert` and `gbuffer.frag`.
- Switch geometry pipeline to load the new shaders and output two color formats.
- Create transient G-buffer textures in the frame graph.
- The geometry pass now writes to G-buffer instead of swapchain.
- **Temporarily**: skip the lighting pass. Add a debug pass that just blits `gbufferAlbedo` to swapchain color so you can visually verify the G-buffer output.

### Step 3 — Lighting pass
- Add `lighting.vert` and `lighting.frag`.
- Create lighting pipeline, descriptor layout, pool, sets, UBO.
- Add `LightingPass` to the frame graph that reads G-buffer and writes swapchain color.
- Remove the temporary debug blit from step 2.
- Verify the scene looks the same as before (same directional light params).

### Step 4 — Wire up RenderLight
- In `Renderer::render`, if `RenderWorld::lights` has a directional light, use its direction/color/intensity for the `LightingUBO`. Otherwise use hardcoded defaults matching the old `triangle.frag` behavior.

### Step 5 — G-buffer debug visualization
- Add `GBufferView` enum, `gbufferView`, and `showBufferOverlay` members to `Renderer`.
- Add push constant struct (viewMode + showOverlay) to `lighting.frag`.
- Implement `sampleBuffer()` helper in the shader for reuse between fullscreen and overlay paths.
- Implement overlay strip logic in the shader: check UV against bottom-strip preview rectangles, sample the appropriate buffer, draw borders.
- Add push constant range (8 bytes, fragment stage) to lighting pipeline desc.
- Push both values in the lighting pass execute callback before the draw call.
- Add `gbufferViewMode` and `showBufferOverlay` to `EditorUI`, thread through `MainMenuBarState`.
- Add "Show Buffer Overlay" checkbox and Lit/Albedo/Normals radio buttons to the Debug menu in `mainmenubar.cpp`.
- Bridge values from `EditorUI` to `Renderer` in the main loop.
- Test: toggle overlay on, confirm three preview rectangles appear at bottom with correct content. Switch fullscreen mode, confirm each buffer fills the viewport. Confirm overlay + fullscreen mode work together (overlay shows all three, main viewport shows selected).

### Step 6 — Cleanup
- Delete `shaders/triangle.vert` and `shaders/triangle.frag` (no longer used).
- Verify debug lines and editor UI still render correctly on top of lit output.
- Run `make format`.

---

## Validation

- Frame graph prints pass order: `GeometryPass → LightingPass → DebugLinePass → EditorUIPass`.
- G-buffer textures transition: `Undefined → ColorAttachment` (geometry pass) → `ShaderReadOnly` (lighting pass) → released.
- Swapchain color transitions: `Undefined → ColorAttachment` (lighting pass) → `ColorAttachment` (debug/UI) → `PresentSrc`.
- Scene appearance matches pre-change (same light direction, color, ambient).
- No validation layer errors.
- `make clean && make` succeeds, new `.spv` files are generated.
