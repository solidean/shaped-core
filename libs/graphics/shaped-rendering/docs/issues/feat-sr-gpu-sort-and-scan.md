---
created: 2026-11-03
priority: next
tags: [gaussian-splats, compute]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/autodiff.md
---

# GPU radix sort and prefix scan routines

Two render routines sr does not have, wanted by the Gaussian splat use case and by much else.

- **A prefix scan** over a buffer of `uint`, exclusive and inclusive.
  Splat tile binning needs it to turn per-splat tile counts into offsets, and stream compaction and GPU-driven culling need the same.
- **A radix sort by key**, key-value pairs of `uint` keys, or 64-bit keys as two words.
  Splat rendering sorts each splat instance by tile id and depth, which is one sort over a combined key, in the forward pass and again for training.
  A sort of a few million pairs per frame is the budget the use case sets.

What is known:

- Both should be SGL, so they run on dx12, vulkan, webgpu and metal from one source.
- SGL has subgroup prefix sums (`subgroup_exclusive_add` and its kin) and workgroup memory with barriers, which is what a decoupled-lookback or reduce-then-scan kernel needs.
- WebGPU's subgroups are a feature a device may lack, so a workgroup-only fallback is mandatory there.
- Onesweep-style decoupled lookback relies on forward-progress guarantees that not every GPU gives, Apple and some mobile parts among them.
  Reduce-then-scan is slower and safe everywhere, so it is likely the default, with lookback where it is known to hold.
  That is a hypothesis to measure, not a decision.
- The size of the sort is GPU-produced, so both routines want an indirect dispatch form.
