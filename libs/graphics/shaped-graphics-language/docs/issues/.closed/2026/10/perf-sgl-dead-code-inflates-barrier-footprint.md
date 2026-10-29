---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/barrier/access_inference.hh
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-20T13:23Z
closed: 2026-10-20
resolution: fixed
---

# A footprint is only as exact as the code the emitter prints

SGL removes no dead code yet, so a use behind a constant-false branch still counts in a pipeline's footprint, and costs the barrier it implies.
Dead-code removal belongs on the tree both the emitter and the footprint read, never in the footprint alone, since an untouched slot records no layout transition.