---
created: 2026-11-13
tags: [half, amd, dx12, vulkan]
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_math.sgl
---

# On AMD a float converted to a half rounds toward zero, as D3D and Vulkan allow, where EVAL-96 rounds to nearest

**This is SGL promising more than its targets do, not AMD's bug.**
D3D's data conversion rules state "Round-to-zero is used during conversion to another float format", and `f32tof16` "follows D3D rules for floating point conversion".
WARP, D3D's reference, rounds both the casts and `f32tof16` toward zero (`docs/bugs-external/intel-float-half-round-trip-folds-away`).
Vulkan leaves a conversion's rounding to the implementation, nearest or toward zero, unless a shader asks for one with `VK_KHR_shader_float_controls`' `RoundingModeRTE`.
So AMD keeps both APIs' rules, and a GPU that rounds to nearest does too; what neither allows is Intel's dx12 casts, which do not round at all.

EVAL-96 rounds every operation of halves once, to nearest, ties to even, and `.fast` licenses no other rounding of a conversion.
On AMD's Radeon iGPU (RDNA 2, dx12 driver 32.0.21042.62, vulkan alike), `f as half` truncates toward zero instead.
Measured by the differential corpus's `half_math.sgl` against exact values: all 256 conversions are the toward-zero half, 127 of them a step from the nearest.
Its half arithmetic is right: `*`, `+` and `-` of the converted values round to nearest on all 256.
DXC's DXIL and SGL's, and DXC's SPIR-V and SGL's, all do it, so it is the driver's conversion and no route's.
Intel's Arc B580 rounds every conversion to nearest.

What would make SGL keep its word:
- vulkan: the `RoundingModeRTE` execution mode for 16-bit floats, where the device grants `shaderRoundingModeRTEFloat16`.
- dx12: no rounding control exists, and toward zero is the API's own rule, so the conversion would round by hand, from the float's bits, on every D3D12 device.
- Or EVAL-96 states the conversion's rounding as nearest or toward zero, the target's, as it does a builtin's precision.
  The interpreter's bounding run would then give a conversion both, and `half_math.sgl` would hold every conforming GPU, AMD's included.

Sources: [Direct3D data conversion rules](https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-data-conversion), [f32tof16](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/f32tof16--sm5---asm-), [VkPhysicalDeviceFloatControlsProperties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFloatControlsProperties.html).

Until then `half_math.sgl`'s `known amd dx12` and `known amd vulkan` lines log the divergence rather than fail it, and fail once AMD agrees.
No refresh is to remove the finding.
