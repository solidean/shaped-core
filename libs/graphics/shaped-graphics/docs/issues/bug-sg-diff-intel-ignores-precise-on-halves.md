---
created: 2026-11-25
refs:
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/shader_asset.cc
  - test: "sg differential - a compile at a precise floor keeps the floor through slib"
  - issue: bug-sg-diff-half-corpus-fails-on-intel
---

# Intel's Arc keeps half operations at a float's precision under a precise floor, on dx12 and vulkan alike

**Not Intel's bug: the floor never reached the GPU's compile.**

The strict-floor twin's first run saw `half_arithmetic.sgl` come back from Intel's Arc at a `.precise` floor bit for bit as at `.fast`, on every route, and this issue blamed the driver.
`shader_asset::acquire` keys, and compiles, with the options the entry point declares and an allowlist of compile options, and `float_floor` was on neither.
So slib dropped it, every twin compile ran at `.fast`, and the twin's own SPIR-V carried no `NoContraction`, as `spirv-dis` of the dumped bytecode showed.

With `float_floor` on the allowlist, Intel rounds every half operation of `half_arithmetic.sgl` as binary16 does, on dx12 and vulkan, both routes.
`docs/bugs-external/intel-float-half-round-trip-folds-away` measures the same on the corpus kernel under `-Gis` and HLSL's `precise`.
`sg differential - a compile at a precise floor keeps the floor through slib` now holds the floor to the GPU's compile.

What the twin does see on Intel is `half_math.sgl`'s round trip folded away on dx12, `bug-sg-diff-half-corpus-fails-on-intel`'s.
