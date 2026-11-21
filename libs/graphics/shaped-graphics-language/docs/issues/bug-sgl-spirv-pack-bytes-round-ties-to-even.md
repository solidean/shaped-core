---
created: 2026-11-20
tags: [spirv, vulkan]
refs:
  - file: libs/graphics/shaped-graphics/tests/compute/compute-pack-routes-test.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/spirv/impl/writer_operations.cc
---

# SGL's SPIR-V packs bytes by GLSL.std.450's round, where EVAL-88 says floor(x + 0.5)

EVAL-88 makes a byte of `pack_unorm4x8` and `pack_snorm4x8` `floor(v + 0.5)` of the scaled value, and HLSL's text helpers write exactly that.
The scaled value and the sum are float arithmetic, so a target may round each to a float or, at `.fast`, fuse them into one multiply-add.
SGL's own SPIR-V writes `PackUnorm4x8` and `PackSnorm4x8` of GLSL.std.450 instead, which round the scaled value with `round`, whose direction at a tie is the implementation's.
So where the product rounds to exactly a byte and a half, a SPIR-V pack may give a byte that neither the unfused nor the fused `floor(v + 0.5)` gives.

Measured on 2026-11-20 by `sg - every half and byte packs and unpacks alike through DXC and through SGL's binary`, on vulkan on an RTX 5080 and on an AMD Radeon iGPU (RDNA 2):
- Both take such a tie to the even byte, where DXC's SPIR-V and both dx12 routes take it up.
  For the float 0x3B008081, 0.5 / 255 rounded, the product rounds to exactly 0.5 and the exact product lies above it, so both readings of EVAL-88 give byte 1; SPIR-V gives 0.
- A rounded product just below a half, 0.49999997, is byte 1 by EVAL-88's unfused sum, which rounds up to 1, and byte 0 by the fused one, as by exact maths.
  SPIR-V gives 0 there, which EVAL-88 allows as the fused result, so that case is no divergence.
Every other byte agrees with EVAL-88 on both drivers.

It is still a bug under that reading of EVAL-88: the tie direction of `round` is a third rounding, which EVAL-88 grants Metal alone.
What would make SGL keep its word: the SPIR-V writer lowers the two packs as the text writes them, `floor(clamp(v) * scale + 0.5)` per lane, as the DXIL writer does.
Or EVAL-88 grants the tie direction to every target whose packs are GLSL.std.450's, as it does Metal's.

`TEMPORARY(bug-sgl-spirv-pack-bytes-round-ties-to-even)` lets the pack-routes test accept the nearest byte on the SPIR-V route alone.
