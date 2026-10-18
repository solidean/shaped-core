---
created: 2026-10-18
refs:
  - file: libs/graphics/shaped-viewer/docs/plans/restir-gi.md
  - symbol: sv::wavefront_trace_desc::lights_key
---

# An emitter that moves drops every ReSTIR DI history

The light table's key covers each emitter's placement, so a moved emitter drops DI's history ([plans/restir-gi.md](../plans/restir-gi.md#di-track)).
The view drops it on its scene hash anyway, so this costs nothing until the view keeps a history across a scene change.
Keeping it then wants the proper shift across placements: the history's resampling weight formed with the placement its point was drawn under.
That needs last frame's instance transforms beside this frame's.
ReSTIR GI's records are the same case: the routine keeps them across a moved instance, and the view forgets them on its scene hash.
