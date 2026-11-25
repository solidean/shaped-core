---
created: 2026-11-25
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_math.sgl
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_arithmetic.sgl
  - test: "sg differential - every corpus entry point*"
  - issue: bug-sg-diff-half-math-unrounded-on-nvidia-vulkan
  - issue: debt-sg-diff-gpu-comparisons-hold-a-conversion-to-one-rounding
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

## Open

- Whether known lines learn a driver (sg's `adapter.driver_name`, `radv`), so RADV's line names this issue and AMD's own driver keeps `across`.
- Whether SGL's SPIR-V decorates a narrowing `OpFConvert` `NoContraction`, which would fix the native route on Mesa; whether it does on NVIDIA's vulkan too is not measured.
- Whether Mesa's fold is one Vulkan allows for an undecorated `OpFConvert`; if not, it is an upstream report like the LDS spills (docs/bugs-external).
