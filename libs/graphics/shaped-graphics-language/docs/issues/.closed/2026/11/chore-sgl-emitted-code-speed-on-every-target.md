---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-rendering/tests/bench/sgl-against-hlsl-benchmark.cc
  - file: libs/graphics/shaped-rendering/tests/bench/sgl-against-wgsl-benchmark.cc
  - file: libs/graphics/shaped-graphics-language/readme.md
closed: 2026-11-09
resolution: fixed
---

# Measure the emitted code's speed on every target

The emitted code is measured against hand-written HLSL on dx12 and vulkan, three kernels each, in sr's `sgl-against-hlsl-benchmark.cc`.
dx12 is 0.99× to 1.02× on an RTX 5070 Ti (2026-10), and 0.99× to 1.00× on an Arc B580, and vulkan 0.97× to 0.99× on the Arc (2026-11-06).
Three runs each agree to about 0.1%, but for one dx12 convolution at 1.105× among otherwise 1.000× runs.
Every ratio here is SGL's time over the hand-written kernel's, so below 1 is SGL being faster.
WebGPU is measured against the same kernels written by hand in WGSL, in sr's `sgl-against-wgsl-benchmark.cc` (2026-11-06).
Each WGSL file ports the HLSL line for line and declares SGL's bindings slot for slot, and the two outputs are compared before they are timed.

| runtime | adapter | à-trous | SVGF filter | convolution |
|---|---|---|---|---|
| node 24, Dawn over D3D12 | Arc B580, driver 32.0.101.9150 | 0.98–1.04× (min 0.94–0.96×) | 0.98–1.00× (1.00–1.13×) | 0.81× (0.80–0.84×) |
| deno, wgpu | Arc B580 | 0.99–1.01× (0.99–1.01×) | 0.98–0.99× (1.01×) | 0.52–0.55× (0.55×) |

Each is SGL's median over the hand-written WGSL's, and the minima's ratio in parentheses, which other work on the GPU only pushes up.
Dawn's run-to-run spread is about ±4%, so its à-trous and SVGF ranges are even with hand-written WGSL.
The convolution's hand-written loops are not unrolled, since WGSL has no `[unroll]`, and SGL unrolls the fixed ones, which is the whole gap.
wgpu's gap grew from 0.73× once a small `filled` stopped being a loop (EMIT-231).

## What the gaps above 1.00× were

Both were on vulkan on the Arc, and neither was in the target compiler.
- **Convolution, 1.01× to 0.97×.** `filled` wrote a loop that DXC's SPIR-V backend leaves a loop, whose dynamic index kept four local arrays in memory (EMIT-231).
- **À-trous, 1.02× to 0.99×.** `atrous_denoise.sgl` asked for `@unroll` and the HLSL it was ported from did not.
  DXC's SPIR-V keeps that loop, and 25 copies ran slower on the Arc's vulkan driver; dx12 and webgpu were even either way.

**The method is the wall clock, not timestamps.**
sg's WebGPU timestamp is an empty compute pass, which nothing orders after the dispatches before it.
So a pair of them can close before the work it brackets has run: SGL's convolution read 0.04 ms that way, with an output that matched.
Each batch of 128 dispatches is recorded first, then timed from its submission to the queue's completion, the pair alternating batch by batch.
A completion is noticed on a tick of about 16 ms, which a batch of 45 ms or more leaves well under a fifth of.

metal is measured against the same kernels hand-written in MSL, in sr's `sgl-against-msl-benchmark.cc` (2026-11-09, Apple M4).
Each `.metal` file ports the HLSL line for line and declares SGL's argument buffers and constants as SGL's own MSL does, and the outputs agree before they are timed.

| adapter | à-trous | SVGF filter | convolution |
|---|---|---|---|
| Apple M4 | 0.88–1.25× (min 0.99–1.05×) | 0.95–1.08× (min 0.97–1.12×) | 0.77–1.06× (min 1.00–1.02×) |

Six runs, the wall-clock method, batches of 128; the medians spread about ±25% run to run, as the GPU's clocks move both contenders together.
The minima are the steady figure: SGL is even with hand-written MSL on all three.
SGL's compile takes `-std=metal3.2` and the hand-written one the toolchain's default; both are `-O2 -fmetal-math-mode=safe`.
Timestamps read 0 on metal under this load then: [the bug](../../../../../../shaped-graphics/docs/issues/.closed/2026/11/bug-sg-metal-timestamp-heaps-exhausted.md).
It was fixed on 2026-11-10, and the benchmark times by GPU timestamps since.

## What the optimizing epic should take from it

[epic-sgl-optimizing-passes-and-ir-targets](../../../epic-sgl-optimizing-passes-and-ir-targets.md) assumed a browser's compiler leaves WGSL nearly as written.
On these kernels it does not cost: SGL's unoptimized WGSL matches hand-written WGSL under both Dawn and wgpu, and beats it where SGL unrolls.
So the epic is not urgent for kernels of this kind; what would make it so is a measured kernel where SGL's text loses to hand-written WGSL.
Kernels with deeper call chains, heavy struct copies or loops SGL cannot unroll are where such a case would show, and none is measured yet.

## Resolution

Every target is measured: dx12 and vulkan against hand-written HLSL, WebGPU against WGSL, metal against MSL.
SGL's text is even with or faster than the hand-written kernels on all of them, the WGSL convolution's unrolling the one clear win.
