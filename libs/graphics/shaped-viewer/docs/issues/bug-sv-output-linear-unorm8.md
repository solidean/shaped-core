---
created: 2026-10-21
tags: [color]
refs:
  - file: libs/graphics/shaped-viewer/shaders/layout.sgl
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/viewer.cc
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/layout_routine.cc
  - issue: feat-sr-tonemapping-routine
  - issue: feat-sv-render-settings-exposure
---

# The viewer writes linear radiance into an 8-bit target: no sRGB encode, no dither

The frame's output is `bgra8_unorm` for the window and the headless capture alike (`viewer.cc`, where the swapchain and the offscreen texture are created).
The layout pass writes the traced `rgba32_float` accumulation into it unchanged: `view_ps` and `wipe_ps` in `layout.sgl` return the sample times the tint.
So the stored bytes are linear radiance, quantized by the fixed-function float-to-unorm conversion, and shown as if they were sRGB.

What that does to an image, seen in a consumer's example captures:

- **Banding in dark gradients.** A linear encoding spends very few of its 256 codes on the dark end, so a navy `sv::background::gradient` steps visibly.
- **Everything reads darker and harsher than its radiance**, so a scene's lights and albedos get tuned against the missing curve, and retuned once it lands.
- **Values above 1 clip** with no tonemap, so a lit near-white surface burns out.

Wanted, in the output pass when the target is 8-bit:

1. Encode linear to sRGB, either in the shader or through an `_srgb` render target view (`layout_routine::execute` already names the window as the seam a per-window color space plugs into).
2. Add a dither of about one code before the quantization, triangular or blue noise, so a smooth gradient stays smooth.
   The capture has to stay byte-stable run to run on one machine, so the noise is a fixed pattern, not drawn per frame.
3. The tonemap of `feat-sr-tonemapping-routine` slots in before both, and is a separate piece of work.

Measured: the formats and the pass above, read in the source on 2026-10-21; the banding, in captures at 1920x1080 and 256 samples.
Not measured: whether any backend creates an sRGB view of the swapchain on its own; the dx12 and vulkan swapchain code only maps the format enum, as far as it was read.
