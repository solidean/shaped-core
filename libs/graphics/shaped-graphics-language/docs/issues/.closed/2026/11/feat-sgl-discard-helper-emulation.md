---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/tests/corpus/control/discard.sgl
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/msl.cc
  - file: libs/graphics/shaped-graphics/docs/tier1-pipeline-tests.md
closed: 2026-11-09
resolution: wontfix
---

# What `discard` does to a quad's derivatives, per target

SGL writes `discard;` and MSL `discard_fragment();`, which every target reads as "no effect after this".
Whether the pixel keeps running as a helper is where they differ, and a sample after a discard in a neighbouring pixel depends on it.
sg's tier-1 pixel-semantics test pins it on every backend, and dx12 and vulkan keep the pixel as a helper.
A target that terminates gets the emulation the design settled: a flag, guarded stores, and the real discard at the end.

Metal is the open one: that test has not run on a Mac, so whether MSL needs the emulation is not known yet ([tier1-pipeline-tests.md](../../../../../../shaped-graphics/docs/tier1-pipeline-tests.md)).

## Resolution

Measured on 2026-11-09 on an M4: `sg - a discarded pixel still takes part in its quad's derivatives` passes on metal.
`discard_fragment` keeps the pixel running as a helper, so its neighbor's derivative reads it, and MSL needs no emulation.
Every target SGL writes now keeps a discarded pixel as a helper; the test runs on each, so a GPU that terminated instead would fail it.
