# Rendering roadmap

What is open after the wavefront tracer landed, in the order we intend to take it.
It spans sv, sr, sg and SGL, and lives here because the viewer is what every item serves.
A library's own TODO keeps the detail of each item; this page keeps the order and the reasons.

## In flight

- **A rasterization path for the PBR materials** — first slice landed as `render_mode::raster` ([raster.md](../raster.md)).
  The same OpenPBR materials are drawn rather than traced, through the generated material body the hit groups use, lit directly and unshadowed.
  Its primary-hit G-buffer lands in the guide slots every frame, which is what a later lighting pass reads.
  Left in it: quadric batches, antialiasing, transparency, and lights seen by the camera.

## The intended default

The wavefront tracer is the default today, and very likely not for good.
The intended default is the hybrid: rasterized primaries, lit by inline traces over few bounces and a radiance cache — [hybrid-renderer.md](hybrid-renderer.md) is its plan.
The wavefront is meant to become the best-quality mode, the one an image converges in where quality matters more than the frame rate.
Neither has landed; the items below are the way there, and [wavefront.md](../wavefront.md#which-tracer-is-the-default) keeps the reasoning.

**The raster mode is meant to look good in its own right**, not only to stand in.
Today it is unshadowed and reflects only the SH environment; a device without ray tracing sees exactly that, and the hybrid builds on it.

**A second raster mode, `raster_bound`, is planned for devices without binding arrays**: WebGPU and phones first.
It binds per material, shades forward, and approximates GI with probes traced through the polyfill — [raster-bound.md](raster-bound.md).

## Decided for the raster path and the hybrid

- **Quality over cost for what is not opaque.**
  Glass gets depth peeling, layered rather than sorted, and foliage-style cutouts get alpha clipping.
  The single cut at one half the first slice makes is the stand-in until then.
- **Ray tracing is the first choice and never a requirement.**
  Interactively the hybrid is the mode: rasterized primaries, lighting by hardware ray tracing or by the ray-query polyfill where there is none.
  Done for raster: a raster layer draws on a device without ray tracing, building no acceleration structure, and only a traced layer acquires the tracer.
  **The polyfill exists on webgpu alone**, where the wavefront is out of scope, so a dx12 or vulkan device without ray tracing has none.
  Bringing it there is an sg item (`feat-sg-ray-query-polyfill-dx12-vulkan`); until it lands, the hybrid there falls back to the raster path's direct lighting.
- **A G-buffer holding all of OpenPBR is too much, and the hybrid takes a thin one instead of a visibility buffer.**
  The raster pass evaluates the material once and writes a reduced closure; [hybrid-renderer.md](hybrid-renderer.md#the-thin-g-buffer) has the layout.
- **No ReSTIR, in any form, until NVIDIA answers on its patents.** The radiance cache comes first, and it must be an open design no patent covers.

## Next, in order

1. **Measure a moving Sponza frame with upscaling on.**
   Before upscaling it was ~64 ms at 1080p; the number decides how urgent everything below is.
2. **The step when the camera stops.**
   The first still frame starts the full-size temporal history over, so it is denoised from one sample.
3. ~~**Ray cones**~~, done: a hit picks a texture's level from its cone (feat-sv-texture-ray-cones, closed).
4. **A design review of [hybrid-renderer.md](hybrid-renderer.md)**, run with the review tool.
   - Interactive: rasterized primaries into a thin G-buffer, GPU-culled, then one inline-traced lighting kernel ending in a radiance cache.
   - Final: the wavefront tracer converging without the cache, with power-weighted light selection.
   - Later: special handling for mirrors and caustics.

## Open, unordered

### Performance

- **The black screen while Sponza loads (~26 s).**
  The guess is that shader compiles queue behind the import on the same workers; not measured.
- **Shading is the most expensive wavefront stage**, about 40% of a still frame.

### Upscaling while moving

- A layer with drawings never upscales: their depth test reads the primary depth at the view's size.
- `dlss_rr` is reached by the scaled frames, but only a hybrid layer's test runs it against them.
- FSR is told a 60 Hz frame time rather than the viewer's own.

### Textures

- A texture evicted from the LRU stays in the bindless table; eviction should release it.
- Compression re-encodes every texture on every load; `bcache` could hold the encoded levels.
- sv compresses everything to BC7, normal maps and single-channel maps included, where BC5 and BC4 fit them.
- sr has no BC6H encoder and no signed BC4 or BC5.
- BC7 searches modes 6, 1, 3 and 7 over two partitions; a wider search is where its remaining quality is.

### SGL

- No array constants yet, so lookup tables live inside functions.
- **`box_filter_mipmap` and FSR's two clear passes are SGL, storing through `v as texel[F]`.**
  Each is compiled once per format it writes, so the mipmap routine is keyed by shape and format, and FSR builds one clear per format it clears.
  Converting a texel back to a fixed width is left for when a shader needs it.
- **NRD's two glue shaders stay HLSL, by decision, and SGL gets no way to call foreign code for them.**
  NRD's license grants modification of its sample code alone and distribution in object code alone, so its shaders cannot be translated into SGL here.
  The NRD member is an HLSL routine around the fetched SDK, compiled by DXC, which is all it needs: NRD itself runs where DXC's output runs.
- `is_nan` and `is_inf` take floats only: WGSL cannot bitcast a lone `f16`, so halves want a test of their own.

### Kept as decided

- Glossy chains still sparkle in the quadric gallery's shadows.
  That stays: the tracer remains unbiased, by decision.

### Housekeeping

- **The viewer's tests take five to six minutes in a debug build**, so its binary carries a 600 s timeout of its own rather than sg's 180 s.
  The time is the unoptimized SGL compiler: a debug build runs these tests 7 to 15 times slower than an optimized one, on identical GPU work.
  Each test compiles its material permutations afresh, since the material shader cache belongs to the test's own resource manager.
  A process-wide compile cache, keyed by source and target, is the fix, and it is deferred; narrowing the tests further gains little, since most are already narrowed.
- Re-test the NVIDIA `InterlockedOr` driver bug on each new driver; the wavefront's counter workaround rests on the shape of its code.
- Whether Sponza gets a committed capture: the asset is a 4 GB on-request fetch, so CI cannot produce one.
