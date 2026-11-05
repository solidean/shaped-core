---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register_math.cc
---

# Maths builtins of a double beyond abs, min, max and clamp

A `double` has `abs`, `min`, `max` and `clamp` (CHK-450), and no `sqrt`, `floor` or transcendental.
DXIL has no double `sqrt` or transcendental, so on dx12 each would be a polyfill to a stated precision; SPIR-V's GLSL.std.450 takes doubles for most of them.
Each is a matrix D5 gap on dx12 and vulkan, which both have doubles.
