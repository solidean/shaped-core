---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics/tests/compute/compute-diagnostic-routes-test.cc
  - file: libs/graphics/shaped-graphics/tests/shaders/sgl_mixed/diagnostic_routes.sgl
  - test: "sg - a compute stage's failed assert reads the same through DXC and through the native route"
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-26T09:19Z
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

## Log

**2026-11-26: AMD's compiler, triggered by SGL's DXIL alone; worked around in SGL's DXIL writer (EMIT-285).**
The body's "both dx12 routes" was the native-DXIL context, where the text route is SGL's DXIL too: DXC's own DXIL stores `{2, 4, 3, 8}` on AMD.
Run in a standalone host, SGL's container stores `{6, 6, 3, 6}` on AMD and right on the Arc and WARP.
Reduced through SGL variants: no print, no message and no debug buffer are needed, only `if not (v != 3): return`.
SGL wrote that as a branch on `xor (icmp ne v, 3), true`, and AMD's compiler then takes `v` as 3 on the path where it is not.
`v * 2 + id.x * 100` stores `{6, 106, 3, 306}`, so it is the constant, not another lane's value.
A negated `v < 3` comes out right, and DXC folds the `not` away at every level, so HLSL never reaches it.
The writer now branches on what a `logical_not` negates, with the ways swapped, and the test runs on AMD again.
[amd-d3d12-negated-equality-branch-substitutes-constant](../../../../../docs/bugs-external/amd-d3d12-negated-equality-branch-substitutes-constant/readme.md) reproduces it.
