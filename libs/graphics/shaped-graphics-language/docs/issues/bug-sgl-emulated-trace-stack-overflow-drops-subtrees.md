---
created: 2026-10-20
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-20T17:57Z
---

# The emulated trace drops a subtree silently when its traversal stack is full

`prelude/raytracing.sgl` pushes both children only `if top + 2 <= 64`, and otherwise pushes nothing and goes on; the TLAS push does the same.
So a BVH deep enough, or a TLAS and a BLAS that share the 64 slots between them, loses hits with no diagnostic.
Found by reading the source and the emitted WGSL, not by a scene that overflows.
A fix counts the overflow where a test can see it, or reserves the slots a well-formed BVH of the pool's depth needs.
It may fold into `debt-sgl-raytracing-prelude-repeated-stack-walk`.
