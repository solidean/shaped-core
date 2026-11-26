---
created: 2026-11-26
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/dxil/impl/writer.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/spirv/impl/writer.cc
  - test: "sgl emit - @unroll(false)*"
  - test: "slib - SGL's DXIL marks each back edge of an @unroll(false) loop*"
closed: 2026-11-26
resolution: fixed
---

# SGL's own DXIL and SPIR-V drop @unroll(false)

`@unroll(false)` asks the target to keep a loop a loop (CHK-414).
The flattener records it on the loop's label (`is_rolled`), and only the HLSL text writer read it, as `[loop]`.
SGL's own DXIL wrote no `!llvm.loop` metadata, and its own SPIR-V wrote `OpLoopMerge ... None`.

Measured by Quest3D (its bug B26-Q389H, 2026-11-26): the HLSL twin of its hybrid ray-tracing raygen marks 14 loops `[loop]`, SGL's DXIL marked none.
Taking the `[loop]`s out of the HLSL twin made its cold pipeline creation about 7% slower, since the driver inlines every trace an unrolled loop copies.

The fix (EMIT-291): the lowering marks a hinted loop's header in the IR (`ir::block::hint`), and the passes carry the mark with the header.
`flatten_branches` moves it to the block that heads the loop once it threads the header away, and refuses to thread a header into another loop's header of a different hint.
DXIL attaches a distinct `!llvm.loop` node naming `llvm.loop.unroll.disable` to every back edge of the loop, as DXC's compile of `[loop]` does, and SPIR-V states `DontUnroll`.

The other forms need nothing: `@unroll` writes no loop at all (CHK-389), and one past the copy limit is an ordinary loop on every route.
`@unroll(n)` is `unsupported-yet` ([feat-sgl-partial-unrolling](../../../feat-sgl-partial-unrolling.md)).
Once it lands, DXIL's `llvm.loop.unroll.count` and SPIR-V's `PartialCount` are its spellings; `PartialCount` is SPIR-V 1.4's, below the 1.6 SGL writes.
