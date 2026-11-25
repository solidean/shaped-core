---
created: 2026-11-25
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_math.sgl
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_arithmetic.sgl
  - test: "sg differential - every corpus entry point*"
  - issue: bug-sg-diff-half-math-unrounded-on-nvidia-vulkan
  - issue: debt-sg-diff-gpu-comparisons-hold-a-conversion-to-one-rounding
closed: 2026-11-25
resolution: fixed
---

# RADV folds a float's round trip through a half, and the known amd vulkan lines take it for AMD's own driver

On RADV, Mesa 26.2.0-devel (git-5c65d4fc31), NAVI33, three differential tests fail on vulkan's dxc and native routes since 994ab100e scoped the `known amd vulkan` lines `across`.
A known line matches by PCI vendor, so RADV and AMD's own vulkan driver are one `amd vulkan`, and they behave differently here.

## Measured

- `half_math.sgl`'s `halves` keeps a float's precision against the interpreter: 1023 of 1024 elements, `0x3efe7280` against `0x3efe8000`.
  These are the values of NVIDIA's vulkan on an RTX 4090 (bug-sg-diff-half-math-unrounded-on-nvidia-vulkan), not AMD's toward-zero rounding.
  Before 994ab100e the file-wide `known amd vulkan` line hid it.
- `half_arithmetic.sgl` agrees with its reference on RADV, so its `known amd vulkan … across` line fails for agreeing, on both routes.
- A four-line HLSL `(float)(float16_t)(x * 0.1f)`, compiled by DXC, has both `OpFConvert`s and no `RelaxedPrecision`; RADV's final NIR keeps only the `fmul`.
- The fold is NIR's, so every Mesa driver shares it: `nir_opt_algebraic.py`, `(('~f2f{s}', ('f2f', a)), ('f2f{s}', a))`, line 1293 on Mesa `main`.
  The `~` applies it to a conversion not marked exact, and a SPIR-V instruction is exact only where it is decorated `NoContraction`.
- `NoContraction` on the outer `OpFConvert`, added by hand, keeps both conversions in RADV's NIR, and the result is rounded (`0x3d4ccccf` becomes `0x3d4cc000`).
  HLSL's `precise` does not do it through DXC, which decorates the `fmul` and neither conversion.
  SPIR-V allows `NoContraction` only on an arithmetic instruction, and `OpFConvert` is a conversion, so this is not an emission to rely on, though `spirv-val` takes it.
- The `RoundingModeRTE 16` execution mode does not keep the pair: RADV still folds it.
- `UnpackHalf2x16(PackHalf2x16(x))` in place of the pair is rounded on RADV, which lowers it to `pack_half_2x16_rtz_split`, toward zero as EVAL-96 allows.
  DXC writes HLSL's `f16tof32(f32tof16(x))` as that pair, so one HLSL spelling would serve both vulkan routes; NVIDIA's vulkan is not measured.
- On RDNA 3 `(x * 0.1) as half as float` is one instruction folded, `v_mul_f32`, and two rounded through `NoContraction`, `v_fma_mixlo_f16` and `v_fma_mix_f32`.
  The pack and unpack is three, since `v_cvt_pkrtz_f16_f32` takes no multiply into it: one more than a driver that rounds as it should.

## Resolved

- EVAL-96 lets `.fast` and `.specials` keep a half at a float's precision, a conversion's too, so the fold is right there and `half_math.sgl` passes against the interpreter at its own level.
- At `.precise` and `.ieee`, sg's vulkan backend grants `float_round_trip_workaround` on a Mesa driver, and EMIT-282 writes the conversion where NIR cannot fold it.
  SGL's own SPIR-V decorates the `OpFConvert` `NoContraction`, and the text writes `float16_t(f16tof32(f32tof16(x)))`; the precise twin passes on both routes.
- A known line's vendor may name Mesa's drivers or its own, `amd/mesa` and `amd/own`, so `half_arithmetic.sgl`'s line names AMD's own driver and RADV is held to the reference.
- Whether Mesa's fold is one Vulkan allows stays open, a question for Mesa and Khronos rather than this tree: SPIR-V gives a conversion no `NoContraction` to refuse it.
