---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/hlsl.cc
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/raytracing_pipeline.cc
---

# A host's groups make a payload's state the widest access

A payload a `.host` pipeline traces is `read`/`write` for every stage on HLSL (EMIT-137).
A group compiled apart cannot agree with the module's inferred qualifiers otherwise.
The way out is compiling the host's groups against the module's inferred qualifiers, which slib would hand to `compile_hit_group`.

hlsl.cc writes `#pragma dxc diagnostic ignored "-Wpayload-access-perf"` into every ray-tracing stage, where only a stage of a widest payload needs it.
