---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-rendering/tests/bench/sgl-against-hlsl-benchmark.cc
  - file: libs/graphics/shaped-graphics-language/readme.md
---

# Measure the emitted code's speed on every target

The emitted code is measured against hand-written HLSL on dx12 and vulkan, three kernels each, in sr's `sgl-against-hlsl-benchmark.cc`.
dx12 is 0.99× to 1.02× on an RTX 5070 Ti and 0.91× to 0.97× on an Arc B580, and vulkan 0.98× to 1.02× on the Arc (2026-11-05).
Nothing measures metal or webgpu, where the text goes through other compilers, and WGSL is emitted unoptimized.
Both need the kernels hand-written in MSL and WGSL to compare against.

The same kernels, hand-written in each target's language, should run against SGL's text on each backend.
WebGPU's numbers decide how urgent [epic-sgl-optimizing-passes-and-ir-targets](epic-sgl-optimizing-passes-and-ir-targets.md) is.
