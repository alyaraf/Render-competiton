# Changes From the Original Code

## Rendering / shader logic

- **Depth of field** — the camera ray is now jittered over a lens disk and re-aimed at a
  fixed focal plane (`aperture`, `focusDist` constants in the shared shader code), instead
  of a single pinhole ray per pixel.
- **Importance-sampled BRDF** — diffuse bounces use cosine-weighted hemisphere sampling and
  specular bounces use GGX importance sampling, instead of the simpler/unweighted sampling
  in the baseline. This converges to a clean image in fewer accumulated frames.
- **Clear-coat material layer** — `microfacetBRDF` / `sampleMicrofacetBRDF` gained a second,
  very smooth specular lobe (fixed IOR 1.5 → F0 = 0.04) layered on top of the base specular
  lobe and energy-conserving with it, instead of a single specular lobe.
- **Procedural materials (superseded)** — DiscoBot, StoneDemon and the VeachPlanes
  (Cornell-box-style) surfaces originally got their base color from animated cosine-gradient
  color palettes driven by position/normal, instead of a flat tint or texture-only lookup.
  Procedural marble/wood noise helpers (`marbleColor`, `woodColor`, `fbm3`, `valueNoise3`,
  `hash13`) were also added to the shared shader code. **All three of these surfaces have
  since been switched to the concrete wall material below**, so the palette code paths are
  currently unused, but the noise helpers remain in `raycommon.glsl`.
- **Concrete wall material on DiscoBot, StoneDemon and VeachPlanes** — all three surfaces
  (`gl_InstanceID` 0, 1 and 2) now sample a PolyHaven "concrete_wall_009" PBR texture set
  (base color, roughness, tangent-space normal map) instead of the procedural cosine
  palettes above, tiled 2× across each UV unwrap via `fract(texCoords * 2.0)`. The sampled
  base color is also multiplied by `0.15` — the raw texture read as washed-out/overexposed
  under this scene's bright area light and the bright HDR sky's indirect bounce lighting (a
  `0.4` multiplier was tried first and still wasn't dark enough). Normal mapping reuses the
  `cotangentFrame` / `applyNormalMap` helpers (added to `raycommon.glsl`) from the original
  baseline shader, which builds a per-triangle tangent frame from the triangle's world-space
  edges and UV deltas rather than needing precomputed vertex tangents.
- **Reduced depth-of-field strength** — `aperture` in `raycommon.glsl` was lowered from
  `0.045` to `0.012`. At `0.045` the lens blur was so strong that the background environment
  was barely recognizable; the lower value keeps a subtle DOF effect while keeping the
  background clearly visible.
- **Street rat material (`gl_InstanceID == 3`)** — a new `else if` branch added to
  `raytraceTri.rchit` / `raytraceAabb.rchit` for the street rat mesh (see "Scene
  composition" below), sampling its own PolyHaven "street_rat" PBR texture set
  (`textureRatDiff`/`Rough`/`Norm`, `textureSamplers[9..11]`) the same way the concrete
  material above does, including the `cotangentFrame`/`applyNormalMap` normal mapping. The
  base color is multiplied by `0.6` (lighter than the concrete material's `0.15`, since the
  rat texture wasn't as overexposed).
- **Soft area light** — the light sample position is jittered over a small sphere each
  frame (soft shadows via Monte-Carlo accumulation), instead of being a fixed point light.
- **HDR sky background** — the miss shader (`shaders/raytrace.rmiss`) now samples an
  equirectangular HDR image (`data/background.hdr`) using the escaped ray's world direction,
  instead of returning a flat near-black ambient color. This changes what camera rays and
  bounce/reflection rays see when they leave the scene. Shadow rays use the same miss shader
  but are unaffected, because `raytraceTri.rchit` / `raytraceAabb.rchit` always overwrite
  `payload.directLight` with computed radiance right after a shadow trace completes. The
  mapping's `v` coordinate was later flipped (`v = 1.0 - acos(d.z) / PI`, was
  `v = acos(d.z) / PI`) because the sky was rendering upside down — the image's top row (sky)
  was being sampled at the bottom of the direction sphere and vice versa. The HDR image
  itself has also been swapped a couple of times (currently a cedar bridge environment) by
  just replacing the contents of `data/background.hdr`; no code changes are needed to swap it.

## Scene composition

- **DiscoBot and StoneDemon scaled down** — `scaleBlas0` lowered from `1.8` to `1.4` and
  `scaleBlas1` from `1` to `0.8` in `data/settings.txt`.
- **DiscoBot and StoneDemon rotated to face each other** — `rotZBlas0` changed from `-70` to
  `-90` and `rotZBlas1` from `0` to `90` in `data/settings.txt`. Both characters' default
  (rotation-0) facing direction was determined empirically by temporarily pointing the
  camera straight down at the scene (`camPos`/`camUp` edited in `raycommon.glsl`, then
  reverted) to see which way each mesh faced before picking the rotation values, since
  there's no other way to query a mesh's authored "forward" axis from this codebase.
- **Street rat added as a 4th object**, standing on the VeachPlanes floor between DiscoBot
  and StoneDemon. Positioned/scaled directly in `main.cpp` (`mesh3T`/`mesh3Scale`, not
  driven by `settings.txt`) at `(0.05, -0.3, -0.96)` with a `10.0` scale factor — an initial
  attempt at `(0.05, -0.6, -0.96)` with scale `4.0` was too small and got partly hidden
  behind the floor's near edge from the camera's angle.

## C++ / asset pipeline

- `HelloVulkan::createTextureImages` (`hello_vulkan.cpp`) gained `.hdr` loading support via
  `stbi_loadf`. The original loader only handled `.pfm` (via `loadPFM`) and 8-bit `.png`
  (via `stbi_load`); HDR pixels are already linear floats and must not be divided by 255 the
  way the 8-bit path does, but do need the same vertical flip to match the shader's sampling
  convention.
- `main.cpp` registers `data/background.hdr` as an additional texture (becoming
  `textureSamplers[5]` / `textureBackground` in the miss shader), alongside the five
  existing material textures (`texture0`-`texture4`).
- `main.cpp` additionally registers the concrete wall texture set as three more textures —
  `data/concreteDiff.jpg`, `data/concreteRough.png`, `data/concreteNorm.png` — becoming
  `textureSamplers[6..8]` / `textureConcreteDiff`/`Rough`/`Norm` in `raytraceTri.rchit` and
  `raytraceAabb.rchit`. The roughness and normal maps started as `.exr` files from the
  source asset pack; since `stb_image` (used by this project's texture loader) can't decode
  OpenEXR, they were converted to 8-bit PNGs offline with `opencv-python` before being added
  to `data/`. The displacement map from that asset pack (`concrete_wall_009_disp_2k.png`) was
  copied into `data/` but is not registered/used — this renderer has no
  displacement/tessellation support.
- **Street rat mesh (`data/mesh3.obj`)** — the source asset (`street_rat_2k.blend`) only
  shipped as a Blender file, and this project's loader only reads `.obj`. Blender was
  installed (`winget install BlenderFoundation.Blender`) and run headless
  (`blender --background --python export_obj.py -- street_rat_2k.blend mesh3.obj`) to
  export it. Its roughness/normal maps were `.exr` like the concrete set above and got the
  same `opencv-python` → PNG conversion (`data/ratRough.png`, `data/ratNorm.png`); the
  diffuse map (`data/ratDiff.jpg`) needed no conversion. Registered as a third
  `additionalTextures` set in `main.cpp` (`ratTextures`), becoming
  `textureSamplers[9..11]` after the concrete set.
- **`HelloVulkan::createTopLevelAS` extended for a 4th object** (`hello_vulkan.cpp`) — this
  function hardcoded exactly 3 instances, each independently toggleable between a triangle
  mesh and a procedural-AABB representation via `settings.modelModeBlas0/1/2` (a leftover
  from the original "intersection shader" tutorial/emulator this project is based on; the
  AABB path is unused here since all three model modes are `1`). The street rat doesn't
  participate in that toggle system — it's appended as one extra, always-triangle
  `VkAccelerationStructureInstanceKHR` after the existing loop, referencing `m_objModel[3]`
  directly, which is what makes it `gl_InstanceID == 3` in the shaders above.
- `HelloVulkan::saveScreenshot` (`hello_vulkan.cpp`/`.h`) is a new method that copies the
  offscreen ray-traced image (`m_offscreenColor`) to a host-visible staging buffer and writes
  it out as a PNG via `stb_image_write`. `main.cpp`'s main loop calls it automatically a
  couple of frames after the path-tracing accumulation finishes (`m_frameCounter` reaches
  `settings.frameSize - 1`), saving to `render_output.png` next to the executable's project
  directory and printing the save path to the console. The short frame delay exists so the
  GPU has definitely finished writing the final accumulated sample before it's copied out.
- **Render resolution decoupled from the window size (bug fix)** — the offscreen ray-traced
  image, the `traceRaysKHR` dispatch extent, the raster fallback's viewport, and
  `saveScreenshot` all used to read `m_size` (the GLFW window/swapchain size). Vulkan clamps
  swapchain extents to fit the monitor, so requesting e.g. `2000x3000` in `settings.txt` on
  a 1920x1080 screen silently produced a ~1924x1055 image with no error. Added a new
  `m_renderSize` (`hello_vulkan.h`/`.cpp`), set once from `settings.launchSizeX/Y` via
  `HelloVulkan::setRenderSize()` (called from `main.cpp` right after `loadSettings()`), and
  switched `createOffscreenRender`, `updateUniformBuffer`'s aspect ratio, `rasterize`'s
  viewport/scissor, the `traceRaysKHR` call, and `saveScreenshot` to use it instead of
  `m_size`. The on-screen preview window still gets clamped to fit the screen (so the live
  window may look stretched/cropped vs. the saved file), but `drawPost` — the pass that
  blits the offscreen image onto the actual swapchain for display — intentionally still uses
  `m_size`, since that one really does need to match the window.

## Build / toolchain

The original project targeted `nvpro_core` and Vulkan SDK 1.3.211 (~2022). Building and
running it against a current Vulkan SDK (1.4.350) instead required three compatibility
fixes, none of which change rendering behavior:

- `vk::DynamicLoader` → `vk::detail::DynamicLoader` in `nvpro_core/nvvk/appbase_vkpp.cpp`,
  since newer Vulkan-Hpp moved this type into the `detail` namespace.
- Disabled the Vulkan Video extension function-pointer wrappers in
  `nvpro_core/nvvk/extensions_vk.cpp` (`#undef VK_KHR_video_queue` and friends), because
  their generated signatures no longer match the struct layouts in the newer Vulkan headers.
  This project never uses video decode/encode, so the extension is simply turned off rather
  than ported.
- `m_device.createRayTracingPipelineKHR(...)` now returns a `vk::ResultValue<vk::Pipeline>`
  instead of being implicitly convertible to `vk::Pipeline`, so the call site in
  `hello_vulkan.cpp` reads `.value` off the result.
