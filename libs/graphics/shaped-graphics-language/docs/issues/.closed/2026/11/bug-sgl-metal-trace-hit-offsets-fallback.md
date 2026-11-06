---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/text_writer.cc
closed: 2026-11-06
resolution: fixed
---

# A metal trace of a world that is no binding member reads group 0's hit offsets

What is wrong or wanted, and what is already known.

On metal a pipeline trace finds its closest-hit records through the hit offsets of the TLAS it traces.
`hit_offsets_of` (`emit/impl/text_writer.cc`) names those only for a binding member or an element of one.
Any other world, a local or a struct field holding it, falls back to `sgl_ctx.hit_offsets->groups[0][0].instances` without a diagnostic.
Then the trace reads the wrong hit group's records, and where group 0 slot 0 holds no TLAS, its mirror entry is 0 and the GPU dereferences null.

Found by a code review on 2026-11-05, read from the code and not reproduced.
The fix is either to name the offsets for every world the flat tree can hold, or to refuse the others as `unsupported-yet` rather than guess.

## Resolution

Measured on 2026-11-06: the fallback was unreachable from source, so no trace read the wrong offsets.
The checker refuses an acceleration structure as a value: a local, a struct field and an `if` choosing between two are all `unsupported-yet`.
A helper's parameter is inlined to the member it stands for, and its trace names that member's group and slot.
`hit_offsets_of` now asserts that invariant instead of returning empty, and only the registry's own checks reach the writer's fallback.
`tests/corpus/types/acceleration-structures.sgl` pins the refusals, and an msl emit test pins the naming, direct, through a parameter and as an element.
