---
created: 2026-11-12
parent: epic-sgl-native-ir-targets
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/float_arithmetic.sgl
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/vector_math.sgl
  - test: sg differential - every corpus entry point computes alike on each route this context takes
---

# SGL's DXIL divides and normalizes unlike DXC's, so one dx12 driver rounds the two routes apart

The differential corpus found it on its first run, on an Intel Arc B580 (dx12 driver 32.0.101.9150).
One dx12 context, the same source down the native route and through DXC, and the two disagree beyond 2 ulps:

- `float_arithmetic.sgl` `rounded_operations`, output 0 alone, `a * b + a / b - (a - b)`: 12 of 256 values, 3 to 16 ulps apart.
- `vector_math.sgl` `vectors`, output 1, a weighted sum of `normalize(cross(p, q) + c)`: 9 of 256 values, 3 to 9 ulps apart.
  One value of its output 2 is 64 ulps apart too, and vulkan sides with the native route on it.

Measured beside it, and agreeing exactly on both routes: `fma`, every rounding, `min`/`max`/`clamp`/`mix`, `sqrt`, `inverse_sqrt`, `1.0 / a`, `dot`, `length`, and every integer entry point.
On vulkan, the native SPIR-V and DXC's agree on all of it, on the same GPU.

A CPU model of the inputs, rounding every operation once, says the native route is the nearer one: thread 13 is exact natively and 5 ulps off through DXC, thread 35 is 4 ulps off against 7.
So neither route is wrong by DX's rules, which let a division be approximate; they are lowered differently, and the driver rounds what each says.

The hypothesis, not yet traced: the two write `fdiv` and `normalize` with different instructions or fast-math flags.
`uv run dev.py run shan -- diff native.dxil dxc.dxil` over the two entry points is the next step.

Until then both files hold their routes to `relative 0.00001` in their comment blocks, naming this issue.
Closing it means the two lower alike, or a decision that they need not, and the override coming out either way.
