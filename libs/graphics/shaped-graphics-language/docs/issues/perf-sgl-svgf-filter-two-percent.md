---
created: 2026-10-22
refs:
  - test: "sr - SGL against the HLSL it was ported from, GPU time per dispatch"
  - file: libs/graphics/shaped-rendering/shaders/svgf.sgl
---

# SVGF's filter runs about 2% slower in SGL than in the HLSL it was ported from

`sr - SGL against the HLSL it was ported from` times one pass of SVGF's filter, `svgf.sgl`'s `atrous_cs` against the HLSL `svgf_atrous.hlsl` it replaced.
Three runs on an RTX 5070 Ti gave 1.023x, 1.021x and 1.018x: about 2%, every time, where à-trous and the convolution are even.
Not looked into yet; comparing the DXIL of the two is the first step.
