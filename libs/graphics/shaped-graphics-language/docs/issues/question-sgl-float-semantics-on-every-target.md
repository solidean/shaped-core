---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/evaluation.md
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
  - test: "sg - an SGL float keeps*"
  - issue: bug-sgl-metal-fast-math
---

# What a float guarantees on every target, beyond metal's

EVAL-6 says a `float` is a 32-bit IEEE number, and metal now compiles SGL's MSL in safe math mode, which keeps NaNs, infinities and signed zeros and does not reassociate.
The other targets still may not.
DXC's SPIR-V declares no `SignedZeroInfNanPreserve`, so a vulkan driver may assume neither arises; WGSL may assume it outright; and DXC reassociates without `-Gis`.
Denormals are the same question on every target, and so is contraction into a fused multiply-add, which shaped-core turns off for C++ but no shader compile does.

`compute-float-specials-test.cc` is what shows a target's behaviour, and it skips every backend but metal until that target's compile asks for the same.
The question is what the spec promises everywhere, and what each target's compile must ask for to keep that promise.
The measured cost of metal's safe math on sv's megakernel was within run-to-run noise, at most about 3% in adjacent runs.
