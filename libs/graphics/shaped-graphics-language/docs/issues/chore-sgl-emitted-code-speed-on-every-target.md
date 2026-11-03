---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-rendering/tests/bench/sgl-against-hlsl-benchmark.cc
  - file: libs/graphics/shaped-graphics-language/readme.md
---

# Measure the emitted code's speed on every target

The emitted code is measured against hand-written HLSL on dx12 alone: 0.99× to 1.02× on three kernels.
Nothing measures vulkan, metal or webgpu, where the text goes through other compilers, and WGSL is emitted unoptimized.

The same kernels, hand-written in each target's language, should run against SGL's text on each backend.
WebGPU's numbers decide how urgent [epic-sgl-optimizing-passes-and-ir-targets](epic-sgl-optimizing-passes-and-ir-targets.md) is.
