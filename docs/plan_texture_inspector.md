# Texture inspector

**Status. Landed.**

## Current state

Material textures carry mip chains (`docs/plan_texture_mipmaps.md`) and the mip level view shows which level a pixel samples
(`docs/plan_mip_debug.md`). Nothing shows the levels themselves: a wrong box filter, a shifted row, garbage in the small levels, or a
broken level offset in the packed upload would only show as a vague colour shift. `FrameGraphPreviews` already blits transient frame
graph textures into ImGui previews, and `ImGuiBackend::registerTexture` takes any `RhiTexture`; the missing piece was a mip level on
`blitTexture`.

## Scope

**In**

- **RHI**: `blitTexture` takes `RhiBlitRegion { mipLevel, extent }` per side and an `RhiFilter`. The old four-argument form stays as a
  non-virtual convenience (level 0, linear). `mipcube` blits its mip 1 into the backbuffer corner with nearest filtering and checks the
  pixel.
- **Inspector** in the Render Debug Scene tab: click a texture row, a panel shows a level slider, the level blitted into a preview that
  fits a 256 box (nearest when magnifying so texels show as blocks), dimensions and bytes of the level, the prims that bind the material
  from the draw log, and a "Dump level" button writing `texture_<material>_L<level>.png` to the working directory.
- **Capture path**: `TextureInspectRequest` travels main to render thread like the draw timing request; `Renderer::recordTextureInspect`
  blits the level after the frame graph into a preview texture the renderer owns, replaced through the deletion queue when the size
  changes; `RenderDebugSnapshot::inspect` carries the ImGui id and level facts back.
- **Verbs**: `inspect <material> <level>` / `inspect off` drives the capture without the window (validation coverage headless);
  `dump-texture <material> <level> <path>` reads a level back through `copyTextureToBuffer` with its `mipLevel` and writes a PNG.
  Obs `TextureDump`; `inspect` block in the render debug JSON.
- Material textures are created with `TransferSrc` usage, which the blit and the readback require.

**Out**

- Channel isolation, histogram, editing, reload. Trigger: a texture bug the level view and dumps cannot explain.
- Array layers in blits. Trigger: the first layered texture in the engine.

## Decisions

- **Preview owned by the renderer, not by `FrameGraphPreviews`.** The frame graph previews are keyed by resource name and take a
  captured frame graph resource; the inspector is one persistent texture with a level. Sharing the class would mean a second entry kind;
  a 60-line capture next to the screenshot path was smaller.
- **Nearest filter when magnifying.** A 4x4 level scaled to 256 with linear filtering is a blur; the point is to see the texels.
- **Dump path defaults to the working directory** from the button, explicit from the verb; same convention as F12 screenshots.

## Verification

- `ngen-example-mipcube --check --validation` passes with the new `blit-mip-1` check. Verified.
- Sponza headless, `inspect 3 2` with `--render-debug --fail-on-validation`: exit 0, render debug JSON `inspect` shows material 3,
  level 2, 1024x1024 of 13. Verified. First run failed on `VUID-vkCmdBlitImage-srcImage-00219`: material textures lacked
  `TransferSrc`, fixed.
- `dump-texture 1 0|6|12`: three PNGs at 4096, 64 and 1 pixels, `TextureDump{ok:true}` each; level 6 is a recognisable 64x64 of the
  stone texture. `dump-texture 99 0` reports not found and exits clean. Verified.
- Windowed: click a texture row, slide levels, the 1x1 is one flat colour near the texture's average; "Bound by" lists the prims.
  Henrik.

## Deferred / follow-ups

- Channel views and a histogram, above.
- Layer selection in `RhiBlitRegion` with the first array texture.
