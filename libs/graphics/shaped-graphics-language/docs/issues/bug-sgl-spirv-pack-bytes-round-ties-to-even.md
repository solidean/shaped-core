---
created: 2026-11-20
tags: [spirv, vulkan]
refs:
  - file: libs/graphics/shaped-graphics/tests/compute/compute-pack-routes-test.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/spirv/impl/writer_operations.cc
---

# SGL's SPIR-V packs bytes by GLSL.std.450's round, where EVAL-88 says floor(x + 0.5)

EVAL-88 makes a byte of `pack_unorm4x8` and `pack_snorm4x8` `floor(v + 0.5)` of the scaled value, and HLSL's text helpers write exactly that.
SGL's own SPIR-V writes `PackUnorm4x8` and `PackSnorm4x8` of GLSL.std.450 instead, whose `round` leaves the direction of a tie to the implementation.
Measured on 2026-11-20 by `sg - every half and byte packs and unpacks alike through DXC and through SGL's binary`, on vulkan on an RTX 5080 and on an AMD Radeon iGPU (RDNA 2):
- Both take a byte tie to the even byte, where DXC's SPIR-V and both dx12 routes take it up.
- Both also round a scaled value just below a half, such as 0.49999997, to the nearest byte, where `floor(v + 0.5)` in float arithmetic rounds the sum up to 1 first.
Every other byte agrees with EVAL-88 on both drivers.

What would make SGL keep its word: the SPIR-V writer lowers the two packs as the text writes them, `floor(clamp(v) * scale + 0.5)` per lane, as the DXIL writer does.
Or EVAL-88 leaves the rounding of a value midway between two bytes to the target, as it does for Metal.

`TEMPORARY(bug-sgl-spirv-pack-bytes-round-ties-to-even)` lets the pack-routes test accept the nearest byte on the SPIR-V route.
