---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/text_writer.cc
---

# A vector spread into a constructor is written component by component

`float4(..lit, 1.0)` is `float4(lit.x, lit.y, lit.z, 1.0)`; the sampled dx12 text holds 53 of them.
Every target takes `float4(lit, 1.0)` and `vec4f(lit, 1.0)`, so a constructor writer could hand a whole spread vector on as one argument.
