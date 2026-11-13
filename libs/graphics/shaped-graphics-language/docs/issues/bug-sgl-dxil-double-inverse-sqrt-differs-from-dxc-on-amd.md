---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics/tests/shaders/sgl_mixed/double_routes.sgl
  - file: libs/graphics/shaped-graphics/tests/compute/compute-double-maths-test.cc
---

# On AMD's dx12 driver, a double's inverse_sqrt differs by an ULP between SGL's DXIL and DXC's

`sg - a double's maths, bits and fma compute alike through DXC and through SGL's binary` fails on the dx12 hardware context of the AMD Radeon iGPU (RDNA 2, driver 32.0.21042.62, `SC_GPU=amd`).
Lane 1 of result 0, `inverse_sqrt(x)`, differs by one ULP between the routes for 29 of the inputs, e.g. x = 4.202036403907526e+135: SGL 1.5426595594715809e-68, DXC 1.542659559471581e-68.
Every other lane agrees to the bit, and Intel's dx12 agrees everywhere.

Both routes compile the same soft helper from `core.sgl`, but not into the same operations.
`native-parity.py diff` on `double_steps`, DXIL: SGL has 17 `fsub` and 3 `fadd` where DXC has 11 and 8, and 17 integer `sub` against DXC's 6.
The fixture runs at `@float(.fast)`, so a rewrite of `a - b` against `a + (-b)` is allowed either side, and AMD's compiler evidently rounds the two shapes differently, e.g. by fusing one into an fma.

Not yet known: which rewrite on our side changes the shape, and whether the result stays within the helper's stated bound on AMD (only the route agreement was checked).
`TEMPORARY(bug-sgl-dxil-double-inverse-sqrt-differs-from-dxc-on-amd)` skips the bit-for-bit route check on an AMD dx12 context.
