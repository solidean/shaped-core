---
created: 2026-11-01
refs:
  - file: libs/graphics/shaped-graphics-language/docs/architecture.md
---

# Optimizing passes, and IR targets beside the text ones

SGL writes readable text today and leaves optimizing to the compiler behind each target.
That holds for DXC and for Metal's toolchain, and it was assumed not to hold for WGSL, compiled with little optimization of its own.
Measured on 2026-11-05, SGL's WGSL matched hand-written WGSL on three sr kernels under Dawn and wgpu, and beat it by unrolling.
[chore-sgl-emitted-code-speed-on-every-target](.closed/2026/11/chore-sgl-emitted-code-speed-on-every-target.md) has the numbers.
So WGSL's need is a kernel where SGL's text measurably loses, not the assumption alone.
The architecture has the place for it: every pass already works on the flat tree, legalized, before a target's emitter reads it.

- **Optimizing passes on the flat tree**, between legalize and emit, opt-in per target and per scenario.
  The usual first set: constant folding and propagation, dead code and dead store elimination, copy propagation, common subexpressions, load forwarding out of GPU memory, and simple loop work.
  WGSL is the target that needs them first; the readable text the other targets get by default stays as it is.
- **IR targets directly**: DXIL and SPIR-V without DXC in between, where a scenario wants that, a runtime compile or a hot reload above all.
  [epic-sgl-native-ir-targets](epic-sgl-native-ir-targets.md) is that work.
- **Fast enough for a runtime compile**: these passes see a legalized tree with no aliasing to speak of, so they can cost a fraction of what DXC or LLVM spend.
  Each pass is measured against the compile it adds to, which `sgl profile` and the compile-profile tests already time.
- **Not every scenario wants them**: a text a human reads, diffs and debugs stays unoptimized, and a frozen bundle (SC_FROZEN_SHADERS) may well take the slow path once.

Nothing is decided beyond that direction; each pass and each IR target becomes a child issue when it is taken up.
