---
created: 2026-11-05
parent: epic-sgl-float-guarantee-ladder
refs:
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/compiler/sgl_compiler.cc
  - test: "sg - an SGL float keeps NaNs, infinities, signed zeros and the order of a sum on the GPU"
closed: 2026-11-13
resolution: fixed
---

# Each target's compile keeps the level its entry point states

Done on 2026-11-05: dx12; metal, whose safe math keeps up to `precise`; and vulkan at every level.
For SPIR-V DXC's `-Gis` writes `SignedZeroInfNanPreserve` and `NoContraction` on every float operation, and `-denorm preserve` writes `DenormPreserve`.
Each stands behind sg's `float_specials` and `float_denormals`, the device's float controls.
Measured on 2026-11-09 on an M4: metal's safe math flushes denormals, read and written, so metal keeps up to `precise` and never `ieee`.
A denormal read in arithmetic counts as zero, a denormal result is zero, and only a copy keeps one; the float-specials test pins it.

dx12: `-Gis` from `specials` on, and `-denorm preserve` for `ieee`, handed to DXC by slib's SGL edge per entry point.
metal: safe math from `specials` on and fast math at `.fast`, which the MSL text states itself as `#pragma METAL fp math_mode` (EMIT-262).
Done on 2026-11-13: the Metal compiler takes the text's pragma over `-fmetal-math-mode` and `MTLCompileOptions::mathMode`.
That was checked both ways on Apple's toolchain, so a metallib and a source compile agree.
Fast and not relaxed, because dx12's `.fast` writes LLVM's whole fast-math set (no NaNs, no infinities) and vulkan's writes none of the preserving modes.
Under fast math Metal's `dfdx` turns coarse (feat-sgl-coarse-derivatives-by-default), and the differential corpus's `float_roots` and `pixel_derivatives` state the cross-machine tolerance that allows.
vulkan: `SignedZeroInfNanPreserve`, and `DenormPreserve` for `ieee`, through `[[vk::ext_execution_mode]]`, each behind the device feature it needs.
The float-specials fixture states `@float(.specials)` and runs on every backend that keeps it, which is the test.
Measured on 2026-11-05: `-Gis` everywhere stalled Intel's driver on sv's pipelines, so only an entry point that asks may pay for it.
