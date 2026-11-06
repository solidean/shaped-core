---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/text_writer.cc
claimed-by: Claude (for Julius Nehring-Wirxel)
claimed-at: 2026-11-06T08:39Z
---

# A metal trace of a world that is no binding member reads group 0's hit offsets

What is wrong or wanted, and what is already known.

On metal a pipeline trace finds its closest-hit records through the hit offsets of the TLAS it traces.
`hit_offsets_of` (`emit/impl/text_writer.cc`) names those only for a binding member or an element of one.
Any other world, a local or a struct field holding it, falls back to `sgl_ctx.hit_offsets->groups[0][0].instances` without a diagnostic.
Then the trace reads the wrong hit group's records, and where group 0 slot 0 holds no TLAS, its mirror entry is 0 and the GPU dereferences null.

Found by a code review on 2026-11-05, read from the code and not reproduced.
The fix is either to name the offsets for every world the flat tree can hold, or to refuse the others as `unsupported-yet` rather than guess.
