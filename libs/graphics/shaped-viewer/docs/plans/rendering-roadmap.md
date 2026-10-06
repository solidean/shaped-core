# Rendering roadmap

What is open after the wavefront tracer landed, in the order we intend to take it.
It spans sv, sr, sg and SGL, and lives here because the viewer is what every item serves.
A library's own TODO keeps the detail of each item; this page keeps the order and the reasons.

## In flight

- **The remaining HLSL shaders move to SGL** — atrous, SVGF and the other HLSL-only sr routines.
  SGL already unrolls loops itself; what it lacks is a way for the source to ask for it on one loop.
  Anything else a port turns out to need is a question first, not a language change made on the way.
- **A rasterization path for the PBR materials.**
  Raster alone for now: the same OpenPBR materials drawn rather than traced.
  It is the base the interactive hybrid mode grows from, so its primary-hit outputs should be what a later lighting pass reads.

## Next, in order

1. **Measure a moving Sponza frame with upscaling on.**
   Before upscaling it was ~64 ms at 1080p; the number decides how urgent everything below is.
2. **The step when the camera stops.**
   The first still frame starts the full-size temporal history over, so it is denoised from one sample.
3. **Ray cones**, so a hit picks a texture's mip level instead of always sampling level 0.
4. **A design review of the rendering modes and the radiance cache**, run with the review tool.
   - Interactive: rasterized primaries, then ray-traced lighting through a radiance cache, possibly on the megakernel.
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
