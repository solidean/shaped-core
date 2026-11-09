---
created: 2026-11-09
priority: next
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/registry.hh
  - symbol: SGL_MSL_MOTION_TAGS
---

# Metal's motion tags double every pipeline trace, moving scene or not

Every MSL intersector, intersection function and table SGL writes carries `instance_motion, primitive_motion`, so a ray-tracing pipeline can trace a moving scene natively.
On a static scene that costs twice the traversal, nearly all of it `instance_motion`'s.

Measured on 2026-11-09 on an M4: 64 instances of a 100k-triangle soup, 1920x1080 threads of four rays each, opaque, GPU time per dispatch from the command buffers.
Kernels alternated round by round, in a standalone Swift program over Metal, so nothing of sg's was timed; two runs each, one quiet and one loaded, gave the same ratios.

| kernel | against a plain `intersector<triangle_data, instancing>` |
|---|---|
| with `instance_motion, primitive_motion`, as SGL writes it | +112% |
| with `instance_motion` alone | +106% |
| with `primitive_motion` alone | +8% |
| `intersection_query`, SGL's inline trace | +3 to 4% |

The pipeline's own shape costs nothing beside that: `world_space_data`, an intersection table over opaque geometry, and closest hit and miss called through visible function tables.
It ran 3.7% faster than an inline `intersection_query` shading the same, and 106% slower with the tags.
So a static scene's pipeline trace on metal pays for motion it never sees, which is the whole of the gap to an inline trace.

The fix is to write the tags only where a moving scene can reach the trace, which needs a gate SGL can see at emit time:

- a package option, as `SOFTWARE_TRACE` is for the software walk, refusing a moving TLAS to a pipeline that did not opt in, as the walk's own groups are refused;
- or the pipeline's TLAS bindings, if a binding can say it moves;
- `primitive_motion` alone is the cheap one, so a gate may keep it and drop `instance_motion` only.

Measured on one scene with trivial shading; a heavier hit group or another GPU may move the ratio, and that is a hypothesis until measured.
