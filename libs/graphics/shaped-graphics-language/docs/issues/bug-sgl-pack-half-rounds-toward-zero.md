---
created: 2026-11-20
tags: [half, dx12, vulkan, amd, nvidia]
refs:
  - file: libs/graphics/shaped-graphics/tests/compute/compute-pack-routes-test.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register_math.cc
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-26T06:57Z
---

# pack_half2x16 rounds toward zero on dx12 and on AMD's vulkan, where SGL rounds to nearest

SGL's interpreter and its constant folder pack a float into the half closest to it, ties to even (`soft_math.hh`'s `half_bits_of`), as the builtin registry's comment says.
The targets do not all agree.
Measured on 2026-11-20 by `sg - every half and byte packs and unpacks alike through DXC and through SGL's binary`.
It packs every half's midpoint to its neighbor and the floats a step either side, two to a word.
95773 of those words hold a value no half holds exactly, and the counts below are of such words:
- dx12 on an RTX 5080 (driver 32.0.16.1088): all 95773 go toward zero, on DXC's DXIL (`f32tof16`) and on SGL's, which writes the same `LegacyF32ToF16`.
- dx12 on an AMD Radeon iGPU (RDNA 2, driver 32.0.21042.62): the same.
- vulkan on that AMD iGPU: the same, on DXC's SPIR-V and on SGL's `PackHalf2x16`.
- vulkan on the RTX 5080: all of them to nearest, ties to even, on both routes.
A value a half holds exactly packs to that half everywhere, and so does an infinity; a NaN packs to a NaN.

It is the target's conversion and no route's: the two routes of each context agree to the bit.
[bug-sgl-half-conversion-rounds-toward-zero-on-amd](.closed/2026/11/bug-sgl-half-conversion-rounds-toward-zero-on-amd.md) is the same finding for `f as half` on AMD.
The corpus's `half_math.sgl` saw no such thing on NVIDIA's dx12, so there `LegacyF32ToF16` truncates where a conversion does not.

What would settle it:
- The spec states `pack_half2x16`'s rounding as the target's, nearest or toward zero.
- Or SGL rounds by hand from the float's bits on dx12, where no rounding control exists, and asks for `RoundingModeRTE` on vulkan where the device grants it.

`TEMPORARY(bug-sgl-pack-half-rounds-toward-zero)` lets the pack-routes test accept a route that packs every such word toward zero, and no route that mixes the two.
