---
created: 2026-11-05
parent: epic-sgl-float-guarantee-ladder
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/float-guarantees.md
closed: 2026-11-05
resolution: fixed
---

# A WebGPU context or pipeline may opt in to weaker float semantics than its shaders state

WebGPU keeps no level above `fast`, so an entry point stating `specials` or more is refused there by default.
A host that accepts the weaker semantics says so on the context or on the pipeline, and its entry points then compile at `fast` with a warning naming what was dropped.
The shader never opts out of its own need: only the code that picked WebGPU knows whether that is acceptable.

The pipeline half is done on 2026-11-05: `shader_asset::acquire` takes `slib::k_weakens_float_option`.
The context half followed the same day: `webgpu_config::weakens_float_guarantees`, which `sg::context::weakens_float_guarantees` reports and every acquire passes.
