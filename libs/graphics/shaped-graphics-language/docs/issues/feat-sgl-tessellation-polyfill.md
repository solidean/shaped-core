---
created: 2026-11-11
parent: epic-sgl-every-stage-everywhere
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/stage-polyfills.md
claimed-by: Claude (for Julius Nehring-Wirxel)
claimed-at: 2026-11-12T08:36Z
---

# The tessellation stages on webgpu and metal, polyfilled

webgpu has no tessellation, and SGL refuses the stages there.
The polyfill is a compute pass: evaluate the control stage's factors, generate each patch's domain locations, evaluate the evaluation stage at each, and draw the buffer.

Metal is this issue's too: Metal 4 has no tessellation, so the native route of [feat-sgl-metal-tessellation](.closed/2026/11/feat-sgl-metal-tessellation.md) is closed.

Open: the grid when factors vary per patch (the largest factor's, or a prefix sum first), and whether a vertex order unlike the native one is acceptable.
Measured against dx12's native tessellation on the same patches.

## Decided

- **Compute, metal first**, since this machine is a Mac; webgpu takes the same passes.
- **D3D's points and order**, ported from Microsoft's reference tessellator (MIT, Mesa's `gallium/auxiliary/tessellator`) into `prelude/tessellation.sgl`, every domain and partitioning.
- **Passes**: A, one thread a patch, runs the vertex stage on its control points and the control stage, and writes the factors.
  B scans each patch's point and index counts into offsets and writes the indirect arguments.
  C, one thread a patch, runs the tessellator into the points and indices at the patch's offset.
  D, one thread a point, runs the evaluation stage; an indexed indirect draw follows, so the draw keeps patch order, then D3D's order within a patch.
- **Capacity by a declared maximum factor**, `max_factor = N` on the control stage, 64 by default, so the scratch is the worst case and nothing is dropped.

## Log

- 2026-11-12: `prelude/tessellation.sgl`, the tessellator as `@internal` SGL over a sink, and `tests/interpret/tessellation-test.cc`.
  339 cases of every domain, partitioning and winding match the reference's counts and its FNV hash over every point and index, on the interpreter.
  The reference was run in a scratch harness; only its counts and hashes are committed.
- 2026-11-12: the control and evaluation stages' compute halves, the vertex half, and the prelude's count, scan and generate passes, which the prelude holds as internal entry points no program lists.
  sg, slib and metal draw it: metal's `sg - a tessellated triangle patch …` and `… quad patch …` tests pass through the polyfill, opt-in by `ctx.set_tessellation_polyfill`.
  Found on the way: slib stated no workgroup size for a compute form of a stage declared otherwise, so metal ran one thread a group; the geometry polyfill had the same flaw unseen.
- 2026-11-12: `@tessellation_control(…, max_factor = N)`, CHK-304; its patches' scratch is sized by it, through slib's emit cache.
  It caps every factor the stage returns, natively too: every return caps each factor, and HLSL states `[maxtessfactor(N)]`.
  So the evaluation stage reads the same factors on every target, which `sg - a control stage's max_factor …` checks on metal through the polyfill.
- 2026-11-16: the scan is one workgroup of 256 threads, a tree scan a chunk of 256 patches at a time with the chunks' totals carried.
  `sg - tessellated patches of differing factors each draw their own points, past one workgroup of the scan` draws 1024 patches of factors 1 to 5 and checks every pixel; an inclusive scan fails it.
  `sg tessellation polyfill - a frame of quad patches, …` measures 10 frames on the M4.
  256 x 256 patches at factor 4 went from 105 ms to 49 ms, 64 x 64 at 16 from 38 to 33, 8 x 8 at 64 from 24 to 21.
  Each idempotent pass run five times instead of once says what it costs a frame.
  The tessellator costs 0.6 ms at 256 x 256 and none measurable at 8 x 8, the evaluation pass 0.8 and 0.06, control and count together 0.12.
  So the thread-a-patch tessellator is kept: a thread a point and a triangle would save at most 0.6 ms a frame of about 5, and none where patches are few and fine.
  About 2 ms a frame is left at 8 x 8 with every pass near zero, likely the benchmark's own pacing (hypothesis).
  Found on the way: every patch reserves its largest factor's scratch, gigabytes for many patches at the default 64 (bug-sg-tessellation-polyfill-scratch-worst-case-per-patch).

## Still open

- dx12 and vulkan: the layout twins that hold the six `sg_tessellation_*` entries, the draw, and the benchmark against their native stages that decides `where_missing`; on Windows.
- webgpu: the control pass reads nine storage buffers with the debug window, over WebGPU's default of eight a stage.
- An indexed draw's `vertex_index` in the vertex stage is the index read, as the geometry polyfill takes it; unchecked against D3D.
