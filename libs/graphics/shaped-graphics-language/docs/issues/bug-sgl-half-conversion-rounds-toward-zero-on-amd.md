---
created: 2026-11-13
tags: [half, amd, dx12, vulkan]
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_math.sgl
---

# On AMD a float converted to a half rounds toward zero, where EVAL-96 rounds to nearest

EVAL-96 rounds every operation of halves once, to nearest, ties to even, and `.fast` licenses no other rounding of a conversion.
On AMD's Radeon iGPU (RDNA 2, dx12 driver 32.0.21042.62, vulkan alike), `f as half` truncates toward zero instead.
Measured by the differential corpus's `half_math.sgl` against exact values: all 256 conversions are the toward-zero half, 127 of them a step from the nearest.
Its half arithmetic is right: `*`, `+` and `-` of the converted values round to nearest on all 256.
DXC's DXIL and SGL's, and DXC's SPIR-V and SGL's, all do it, so it is the driver's conversion and no route's.
Intel's Arc B580 rounds every conversion to nearest.

What would make SGL keep its word:
- vulkan: the `RoundingModeRTE` execution mode for 16-bit floats, where the device grants `shaderRoundingModeRTEFloat16`.
- dx12: no rounding control exists, so the conversion would round by hand, from the float's bits, or correct the driver's half by one step where it truncated.
- Or EVAL-96 states the conversion's rounding as the target's, as it does a builtin's.

Until then `half_math.sgl` fails its reference on AMD, which is the finding and no refresh's to remove.
