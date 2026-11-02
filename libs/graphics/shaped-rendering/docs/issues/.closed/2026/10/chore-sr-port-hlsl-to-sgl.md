---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/CMakeLists.txt
  - file: libs/graphics/shaped-rendering/shaders/
  - symbol: sr::query_reconstruct_support
closed: 2026-10-17
resolution: fixed
---

# Port the rest of sr's HLSL to SGL

Blit, imgui, the raster mip filter and the network's pool and upsample are ported.
What holds each remaining shader back is SGL's, and none of it is a blocker so much as a workaround to mark:
- à-trous and the SVGF passes want vector comparisons with `any`/`all`, integer-vector `min`/`max`/`clamp`, `const` arrays, and `use` for the shared SVGF helpers;
- `nn_input` and `nn_output` want `is_nan`/`is_inf`, and `nn_conv` an unroll hint before its tuned loop is trusted to SGL's HLSL;
- the compute box-filter mipmap writes whatever format its texture has, which an SGL image cannot say without a type parameterized on the format.

Every denoise member also writes the caller's `output` as an image, whose format an SGL image must name, so porting them means pinning that format.
Until the network is whole in SGL, `sr::query_reconstruct_support` answers false on webgpu and metal.

Porting also removes the hand-written "find the constants_buffer binding" loops the members and the playground example use to build their pipeline layouts.
An SGL entry point states its layout (`acquire_layout`).
