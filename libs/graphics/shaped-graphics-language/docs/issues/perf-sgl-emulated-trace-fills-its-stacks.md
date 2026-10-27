---
created: 2026-10-22
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
---

# The emulated trace fills both 64-entry stacks on every trace

`trace_emulated` and its procedural and mixed siblings start every trace with `uint[64].filled(0)` and `int[64].filled(-1)`.
That is 128 stores in WGSL before the walk begins, where the walk reads only entry 0 before it writes it.
WGSL zeroes a `var` already, so `nodes` needs no fill there at all, and `owners` needs only `owners[0] = -1`.
The prelude cannot say so yet: `undefined()` gives a value nobody may read, and an array whose elements are written before they are read is no such value.
Seen in the codegen survey of 2026-10-22, on `raytracing.sgl`'s `trace_cells`.
