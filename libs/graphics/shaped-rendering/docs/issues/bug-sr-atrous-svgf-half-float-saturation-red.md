---
created: 2026-11-13
refs:
  - test: "sr - atrous and svgf saturate a pixel past a half float's range in a half-float output"
---

# atrous and svgf no longer saturate a pixel past a half float's range in a half-float output

**The svgf member saturates no pixel: the check `saturated == k_size * k_size` reads 0 == 1024.**
Red on dx12 native-DXIL, vulkan native-SPIR-V and vulkan, at `0507d7ace` and after it; seen 2026-11-13 on an Arc B580.

Not known: whether atrous fails as well or only svgf reached the check, and which change made it red.
