---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/evaluation.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
  - test: "sg - an SGL float keeps*"
  - issue: bug-sgl-metal-fast-math
closed: 2026-11-05
resolution: fixed
---

# What a float guarantees on every target, beyond metal's

EVAL-6 says a `float` is a 32-bit IEEE number, and metal now compiles SGL's MSL in safe math mode, which keeps NaNs, infinities and signed zeros and does not reassociate.
The other targets still may not.
DXC's SPIR-V declares no `SignedZeroInfNanPreserve`, so a vulkan driver may assume neither arises; WGSL may assume it outright; and DXC reassociates without `-Gis`.
Denormals are the same question on every target, and so is contraction into a fused multiply-add, which shaped-core turns off for C++ but no shader compile does.

`compute-float-specials-test.cc` is what shows a target's behaviour, and it skips every backend but metal until that target's compile asks for the same.
The question is what the spec promises everywhere, and what each target's compile must ask for to keep that promise.
The measured cost of metal's safe math on sv's megakernel was within run-to-run noise, at most about 3% in adjacent runs.

## dx12 with `-Gis`, measured 2026-11-05

On an Intel Arc B580, driver 32.0.101.9150, the float-specials probe was run with and without DXC's `-Gis` added for dx12.
Without it, DXC marks every float operation `fast`: `0 / 0` is no NaN, `(1e30 + 1) - 1e30` reassociates to 1, and all three NaN comparisons come out wrong.
With it, the DXIL is IEEE-strict, no `fast` flags and the NaN tests kept, and every check passes but one.
That one is `inf * 0`, a strict `fmul` in the DXIL that the driver returns as 0, which suggests its own compiler folds `x * 0`; not reproduced standalone.
`-Gis` was not landed: shaped-viewer-test then hung for over 170 s inside igd12um64xe2.dll creating sv's pipelines, where without it the binary passes in 38 s.
So dx12 needs either a driver that compiles strict DXIL in time, or `-Gis` opt-in per shader or per vendor, and that is the decision this question now holds for dx12.
Vulkan needs the `SignedZeroInfNanPreserve` execution mode, which DXC can write through `[[vk::ext_execution_mode]]` behind a device feature check; WebGPU offers no switch at all.

## The answer, agreed 2026-11-05

Not one promise for every shader, but a ladder an entry point climbs where it needs to: `fast`, `specials`, `precise` and `ieee`, with `fast` the default.
[incubator/float-guarantees.md](../../../../spec/incubator/float-guarantees.md) is the design, and [epic-sgl-float-guarantee-ladder](../../../epic-sgl-float-guarantee-ladder.md) the work.
This question closes when the epic's first child lands.
