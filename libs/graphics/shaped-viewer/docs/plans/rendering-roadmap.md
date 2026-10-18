# Rendering roadmap

What is open after the wavefront tracer landed, in the order we intend to take it.
It spans sv, sr, sg and SGL, and lives here because the viewer is what every item serves.
A library's own TODO keeps the detail of each item; this page keeps the order and the reasons.

## In flight

- **A rasterization path for the PBR materials** — first slice landed as `render_mode::raster` ([raster.md](../raster.md)).
  The same OpenPBR materials are drawn rather than traced, through the generated material body the hit groups use, lit directly and unshadowed.
  Its primary-hit G-buffer lands in the guide slots every frame, which is what a later lighting pass reads.
  Left in it: quadric batches, antialiasing, transparency, lights seen by the camera, and a depth prepass.

## The intended default

The wavefront tracer is the default today, and very likely not for good.
The intended default is the hybrid: rasterized primaries, lit by the megakernel, which is the cheaper tracer on the shallow paths lighting needs.
The wavefront is meant to become the best-quality mode, the one an image converges in where quality matters more than the frame rate.
Neither has landed; the items below are the way there, and [wavefront.md](../wavefront.md#which-tracer-is-the-default) keeps the reasoning.

**The raster mode is meant to look good in its own right**, not only to stand in.
Today it is unshadowed and reflects only the SH environment; a device without ray tracing sees exactly that, and the hybrid builds on it.

## Decided for the raster path and the hybrid

- **Quality over cost for what is not opaque.**
  Glass gets depth peeling, layered rather than sorted, and foliage-style cutouts get alpha clipping.
  The single cut at one half the first slice makes is the stand-in until then.
- **Ray tracing is the first choice and never a requirement.**
  Interactively the hybrid is the mode: rasterized primaries, lighting by hardware ray tracing or by the ray-query polyfill where there is none.
  Done for raster: a raster layer draws on a device without ray tracing, building no acceleration structure, and only a traced layer acquires the tracer.
- **A G-buffer holding all of OpenPBR is too much.**
  A visibility buffer is the approach to try first — instance and primitive per pixel, the hit rebuilt where it is lit, which is the wavefront's own hit record.
  Its cost is real, so it is the first candidate rather than the decision; the design review weighs it.

## Next, in order

1. **Measure a moving Sponza frame with upscaling on.**
   Before upscaling it was ~64 ms at 1080p; the number decides how urgent everything below is.
2. **The step when the camera stops.**
   The first still frame starts the full-size temporal history over, so it is denoised from one sample.
3. **Ray cones**, so a hit picks a texture's mip level instead of always sampling level 0.
4. **A design review of the rendering modes and the radiance cache**, run with the review tool.
   - Interactive: rasterized primaries, then ray-traced lighting through a radiance cache, possibly on the megakernel.
     The raster path's G-buffer is its input; what it lacks for that is the material itself, likely as a visibility buffer ([raster.md](../raster.md#the-g-buffer-seam)).
   - Final: the wavefront tracer converging without the cache.
   - Later: special handling for mirrors and caustics.

## Open, unordered

### Performance

- **The black screen while Sponza loads (~26 s).**
  The guess is that shader compiles queue behind the import on the same workers; not measured.
- **Shading is the most expensive wavefront stage**, about 40% of a still frame.

### Upscaling while moving

- A layer with drawings never upscales: their depth test reads the primary depth at the view's size.
- `dlss_rr` is not reached by the scaled frames.
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
