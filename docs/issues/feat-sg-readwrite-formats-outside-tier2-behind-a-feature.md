---
created: 2026-11-24
priority: normal
tags: [sg, sgl, webgpu, metal]
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/context/capabilities.hh
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_resources.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/emit.cc
  - symbol: sg::feature::readwrite_image_formats
---

# A read-write image in a format WebGPU's and Metal's tier 2 leave out passes every check and fails at pipeline creation

`sg::feature::readwrite_image_formats` stands for read-write images in any format, and WebGPU grants it with `texture-formats-tier2`, Metal with read-write texture tier 2.
Neither tier covers every format sg has: WebGPU's tier-2 read-write list names no `rg` format, no snorm format, and neither `rgb10a2_unorm`, `rg11b10_float` nor `bgra8_unorm`.
WebGPU's list, besides core's three `r32` formats: `r8unorm`, `r8uint`, `r8sint`, `rgba8unorm`, `rgba8uint`, `rgba8sint`,
`r16uint`, `r16sint`, `r16float`, `rgba16uint`, `rgba16sint`, `rgba16float`, `rgba32uint`, `rgba32sint` and `rgba32float`.
Apple's read-write tier 2 list is close to it.

So a `mut image_2d[.rg16_float]` checks with `require readwrite_image_formats`, sg grants the feature on a tier-2 device, and the pipeline fails in WebGPU's or Metal's own validation.

Julius' principle (2026-11-24): what only some backends have is behind a proper feature flag.
The 16-bit normalized formats took that route: `sg::feature::readwrite_norm16_image_formats`, which neither API grants, and which SGL's EMIT-109 refuses for `wgsl` and `msl`.

## Wanted

- The formats outside both tier-2 lists are named, and their read-write image is a feature of its own (or the norm16 one widened and renamed), which webgpu and metal never grant.
- SGL needs that feature for a `mut` image of them (CHK-201), and EMIT-109 refuses it for `wgsl` and `msl`.
- sg refuses a read-write layout of them without the feature, in the shared layer, as it refuses the norm16 ones.
- Whether Metal's and WebGPU's lists differ enough to need two features is checked against both, not assumed.
