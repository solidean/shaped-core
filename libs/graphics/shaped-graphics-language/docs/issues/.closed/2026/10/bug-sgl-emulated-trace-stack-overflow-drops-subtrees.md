---
created: 2026-10-20
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-20T17:57Z
closed: 2026-10-20
resolution: wontfix
---

# The emulated trace drops a subtree silently when its traversal stack is full

`prelude/raytracing.sgl` pushes both children only `if top + 2 <= 64`, and otherwise pushes nothing and goes on; the TLAS push does the same.
So a BVH deep enough, or a TLAS and a BLAS that share the 64 slots between them, loses hits with no diagnostic.
Found by reading the source and the emitted WGSL, not by a scene that overflows.
A fix counts the overflow where a test can see it, or reserves the slots a well-formed BVH of the pool's depth needs.
It may fold into `debt-sgl-raytracing-prelude-repeated-stack-walk`.

Not reachable, it turned out, on 2026-10-20.
The webgpu pool's trees are balanced BVH2s with at most four primitives a leaf (`lay_out_tree` in sg's `webgpu_raytracing.cc`).
A walk pushes one node per level it descends, and a TLAS leaf at most four instances, so the stack holds at most both depths and 4.
The pool binds at most 128 MiB, 2^23 units, and a tree of depth D takes at least 2^(D+1) of them, so a TLAS and its BLASes sum to 42 levels at most.
That is 46 entries of the 64, which `raytracing.sgl` now says where the stack is declared.
A builder that made unbalanced trees, or a larger pool, would reopen it.
