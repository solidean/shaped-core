---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-rendering/tests/bench/sgl-against-hlsl-benchmark.cc
  - file: libs/graphics/shaped-rendering/tests/bench/sgl-against-wgsl-benchmark.cc
  - file: libs/graphics/shaped-graphics-language/readme.md
---

# Measure the emitted code's speed on every target

The emitted code is measured against hand-written HLSL on dx12 and vulkan, three kernels each, in sr's `sgl-against-hlsl-benchmark.cc`.
dx12 is 0.99× to 1.02× on an RTX 5070 Ti and 0.91× to 0.97× on an Arc B580, and vulkan 0.98× to 1.02× on the Arc (2026-11-05).
WebGPU is measured against the same kernels written by hand in WGSL, in sr's `sgl-against-wgsl-benchmark.cc` (2026-11-05).
Each WGSL file ports the HLSL line for line and declares SGL's bindings slot for slot, and the two outputs are compared before they are timed.

| runtime | adapter | à-trous | SVGF filter | convolution |
|---|---|---|---|---|
| node 24, Dawn over D3D12 | Arc B580, driver 32.0.101.9150 | 1.00× (min 1.02×) | 1.00× (0.99×) | 0.81× (0.82×) |
| deno, wgpu | Arc B580 | 1.00× (1.00×) | 1.00× (1.00×) | 0.73× (0.75×) |

Each is SGL's median over the hand-written WGSL's, and the minima's ratio in parentheses, which other work on the GPU only pushes up.
The convolution's hand-written loops are not unrolled, since WGSL has no `[unroll]`, and SGL unrolls the fixed ones, which is the whole gap.

**The method is the wall clock, not timestamps.**
sg's WebGPU timestamp is an empty compute pass, which nothing orders after the dispatches before it.
So a pair of them can close before the work it brackets has run: SGL's convolution read 0.04 ms that way, with an output that matched.
Each batch of 128 dispatches is recorded first, then timed from its submission to the queue's completion, the pair alternating batch by batch.
A completion is noticed on a tick of about 16 ms, which a batch of 45 ms or more leaves well under a fifth of.

Left: metal, against the kernels hand-written in MSL, which is Mac work.

## What the optimizing epic should take from it

[epic-sgl-optimizing-passes-and-ir-targets](epic-sgl-optimizing-passes-and-ir-targets.md) assumed a browser's compiler leaves WGSL nearly as written.
On these kernels it does not cost: SGL's unoptimized WGSL matches hand-written WGSL under both Dawn and wgpu, and beats it where SGL unrolls.
So the epic is not urgent for kernels of this kind; what would make it so is a measured kernel where SGL's text loses to hand-written WGSL.
Kernels with deeper call chains, heavy struct copies or loops SGL cannot unroll are where such a case would show, and none is measured yet.
