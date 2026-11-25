---
created: 2026-11-20
tags: [spirv, vulkan]
refs:
  - file: libs/graphics/shaped-graphics/tests/compute/compute-pack-routes-test.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/spirv/impl/writer_operations.cc
closed: 2026-11-25
resolution: fixed
---

# SGL's SPIR-V packs bytes by GLSL.std.450's round, where EVAL-88 says floor(x + 0.5)

EVAL-88 makes a byte of `pack_unorm4x8` and `pack_snorm4x8` `floor(v + 0.5)` of the scaled value, and HLSL's text helpers write exactly that.
The scaled value and the sum are float arithmetic, so a target may round each to a float or, at `.fast`, fuse them into one multiply-add, and both give the same byte (EVAL-88).
SGL's own SPIR-V writes `PackUnorm4x8` and `PackSnorm4x8` of GLSL.std.450 instead, which round the scaled value to the nearest byte with `round`, whose direction at a tie is the implementation's.
So a SPIR-V pack may give a byte `floor(v + 0.5)` never gives, fused or not: where the product rounds to a byte and a half, and where it rounds to just below one.

Measured on 2026-11-20 by `sg - every half and byte packs and unpacks alike through DXC and through SGL's binary`, on vulkan on an RTX 5080 and on an AMD Radeon iGPU (RDNA 2):
- Both take a tie of the rounded product to the even byte, where DXC's SPIR-V and both dx12 routes take it up.
  For the float 0x3B008081, 0.5 / 255 rounded, the product rounds to exactly 0.5, so EVAL-88's byte is 1; SPIR-V gives 0.
- Both take a rounded product just below a half to the nearer byte, 0.
  For the float 0x3B008080 the product is exactly 0.5 - 2^-25, and the sum 1 - 2^-25 rounds up to 1 unfused and fused alike, so EVAL-88's byte is 1.
Every other byte agrees with EVAL-88 on both drivers.

Both are the same divergence: GLSL.std.450 rounds to the nearest byte, which EVAL-88 grants Metal alone, and only at a tie.
What would make SGL keep its word: the SPIR-V writer lowers the two packs as the text writes them, `floor(clamp(v) * scale + 0.5)` per lane, as the DXIL writer does.
Or EVAL-88 grants the nearest byte to every target whose packs are GLSL.std.450's, more than it grants Metal.

`TEMPORARY(bug-sgl-spirv-pack-bytes-round-ties-to-even)` lets the pack-routes test accept the nearest byte on the SPIR-V route alone.
