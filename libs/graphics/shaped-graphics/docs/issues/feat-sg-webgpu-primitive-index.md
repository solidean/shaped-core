---
created: 2026-11-09
refs:
  - file: libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_context.hh
parent: epic-sv-raster-bound
---

# Request WebGPU's primitive-index where the adapter offers it

The webgpu backend reports `sg::feature::primitive_index` as false unconditionally, because the emdawnwebgpu it builds against does not request the `primitive-index` extension.
So sv's bound raster mode draws pulled on every browser, Chrome and Edge included, which offer the feature.

Wanted: request it where the adapter offers it, and report it then.
Pulled draws stay the path for Firefox and anywhere else without it; whether indexed draws are faster there is unmeasured.
