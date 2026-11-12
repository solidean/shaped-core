---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics/tests/compute/compute-diagnostic-routes-test.cc
  - file: libs/graphics/shaped-graphics/tests/shaders/sgl_mixed/diagnostic_routes.sgl
  - test: "sg - a compute stage's failed assert reads the same through DXC and through the native route"
---

# On AMD's dx12 driver, a value read before a failed assert's branch is the failing lane's after it

On the AMD Radeon iGPU (RDNA 2, driver 32.0.21042.62), `stamped` in `diagnostic_routes.sgl` stores wrong values on dx12.
The four threads read `{1, 2, 3, 4}`, print, assert `v != 3`, and store `v * 2`; the thread that holds 3 returns before its store.
dx12 stores `{6, 6, 3, 6}`: every surviving thread stores the failing thread's `v` doubled, where `{2, 4, 3, 8}` is right.

Measured, `SC_GPU=amd`:

- Both dx12 routes do it, DXC's HLSL and SGL's own DXIL alike, so the two route-agreement checks pass and the value check fails.
- AMD's vulkan, both routes, stores `{2, 4, 3, 8}`, and so do Intel's dx12 and vulkan.
- The HLSL `sgl emit --diagnostics` writes for it is plain: `v` is a `const int` read once, the branch only reads it, and the store after the branch uses it.

So it is very likely AMD's dx12 compiler, losing a per-lane value across a divergent branch that writes the debug buffer and returns.
Not yet confirmed: no standalone reproduction, and no variant (no print, no return, no debug buffer) has been tried.

`TEMPORARY(bug-sg-amd-dx12-assert-branch-clobbers-value)` skips the test on an AMD dx12 context.
Close it once a reduction says whose bug it is: a `docs/bugs-external/` entry if AMD's, a fix if the debug window's binding is ours.
