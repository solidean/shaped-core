# Rendering roadmap

What is open after the wavefront tracer landed, in the order we intend to take it.
It spans sv, sr, sg and SGL, and lives here because the viewer is what every item serves.
A library's own TODO keeps the detail of each item; this page keeps the order and the reasons.

## In flight

- **A rasterization path for the PBR materials** — first slice landed as `render_mode::raster` ([raster.md](../raster.md)).
  The same OpenPBR materials are drawn rather than traced, through the generated material body the hit groups use, lit directly and unshadowed.
  Its primary-hit G-buffer lands in the guide slots every frame, which is what a later lighting pass reads.
  Left in it: quadric batches, antialiasing, transparency, lights seen by the camera, and a depth prepass.

## Decided for the raster path and the hybrid

- **Quality over cost for what is not opaque.**
  Glass gets depth peeling, layered rather than sorted, and foliage-style cutouts get alpha clipping.
  The single cut at one half the first slice makes is the stand-in until then.
- **Ray tracing is the first choice and never a requirement.**
  Interactively the hybrid is the mode: rasterized primaries, lighting by hardware ray tracing or by the ray-query polyfill where there is none.
  So `view_renderer` must stop requiring the path tracer to come up before a raster frame can draw.
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
- **`box_filter_mipmap` and FSR's two clear passes are HLSL until SGL can convert a value to an image's format.**
  Each stores a `float4` into an image of whatever format the host picks.
  SGL types an image's value by its format, so one body cannot store into `r16_float` and `rgba16_float` alike.
  The direction is an explicit conversion to the target's texel, dropping what the format has no channel for, rather than a store that does it silently.
  Its spelling is open.
- **NRD's two glue shaders stay HLSL, by decision, and SGL gets no way to call foreign code for them.**
  NRD's license grants modification of its sample code alone and distribution in object code alone, so its shaders cannot be translated into SGL here.
  The NRD member is an HLSL routine around the fetched SDK, compiled by DXC, which is all it needs: NRD itself runs where DXC's output runs.
- `is_nan` and `is_inf` take floats only: WGSL cannot bitcast a lone `f16`, so halves want a test of their own.

### Kept as decided

- Glossy chains still sparkle in the quadric gallery's shadows.
  That stays: the tracer remains unbiased, by decision.

### Housekeeping

- **The viewer's tests take about 4.5 minutes in a debug build**, so its binary carries a 360 s timeout of its own rather than sg's 180 s.
  The expensive tests should narrow themselves under `nx::is_thorough()` and the allowance go back.
- **`shaped-rendering-test` hung once in the `debug-nopch` leg of `check`**, killed at 180 s with nothing on stderr.
  Eight repeats of that binary alone in the same preset passed in about 25 s each, so it is unreproduced.
- Re-test the NVIDIA `InterlockedOr` driver bug on each new driver; the wavefront's counter workaround rests on the shape of its code.
- Whether Sponza gets a committed capture: the asset is a 4 GB on-request fetch, so CI cannot produce one.
