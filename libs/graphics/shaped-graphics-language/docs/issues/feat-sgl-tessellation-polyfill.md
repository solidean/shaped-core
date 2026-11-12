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

## Still open

- Passes A to D, their reserved bindings, slib acquiring the pipelines, and metal recording the dispatches ahead of the draw.
- C is serial per patch, 8192 triangles at factor 64 on a quad; a thread per point and per triangle is the faster form, once measured.
- Measured against dx12's native tessellation on the same patches, on Windows.
