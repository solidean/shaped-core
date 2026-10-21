---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/pathtrace_routine.cc
---

# A pipeline is keyed on the whole permutation SET, in scene order

Two views whose scenes hold the same materials in a different order build two pipelines over the same shaders.
Sorting the set before keying it would collapse them, at the cost of a `hit_group_offset` that no longer follows first use.
Worth doing once a scene has enough materials for it to matter.
