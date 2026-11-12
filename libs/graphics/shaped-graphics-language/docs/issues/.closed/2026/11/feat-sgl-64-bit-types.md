---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
closed: 2026-11-05
resolution: fixed
---

# 64-bit integers and doubles

Done on 2026-11-05: `long`, `ulong`, `double` and their vectors (CHK-450 to CHK-453), behind `shader_int64` and `shader_f64`.
dx12 and vulkan run both, shown by sg's compute test; metal has `long` and `ulong` and refuses the double (EMIT-192), and WebGPU refuses both.

Left, each its own issue when a shader wants it:

* Maths functions of a double beyond `abs`, `min`, `max` and `clamp`; DXIL has no double `sqrt` or transcendental, so they would be polyfills.
* Subgroup operations over 64-bit values, which dx12 and vulkan have.
* A `ulong` literal past 2^63 - 1, since an integer literal is held in an `i64`.
* A 64-bit value on a stage edge or in an `@inline` binding, and a 64-bit three- or four-vector in a constant block.
* The metal arm, compiled and run on Apple hardware.
