---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/register_double_math.cc
closed: 2026-11-06
resolution: fixed
---

# Maths builtins of a double beyond abs, min, max and clamp

Done on 2026-11-06 (CHK-458, EVAL-108, EMIT-219): sqrt, the roundings, mix, smoothstep, the vector geometry and the rest, on dx12 and vulkan.
sg's `compute-double-maths-test.cc` runs them against the CPU's IEEE values.
DXC turned out to narrow a double handed to its own `sqrt`, `floor` or `lerp` to a float, on SPIR-V as on DXIL, so each is HLSL over the double's bits, arithmetic and `fma`.
No transcendental takes a double: no target computes one in 64 bits, and the miss says so.

A `double` has `abs`, `min`, `max` and `clamp` (CHK-450), and no `sqrt`, `floor` or transcendental.
DXIL has no double `sqrt` or transcendental, so on dx12 each would be a polyfill to a stated precision; SPIR-V's GLSL.std.450 takes doubles for most of them.
Each is a matrix D5 gap on dx12 and vulkan, which both have doubles.
