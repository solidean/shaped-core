---
created: 2026-10-17
parent: epic-sv-layout-interaction
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/impl/view_state.hh
  - symbol: sv::view_ref::movable
---

# A lifted view cannot be resized, or put back: which gestures?

`view_ref::movable` + Ctrl+left-drag floats a view over the window.
There is no grab handle to resize it and no gesture to drop it back into the flow.
Clearing `view_state::placement_seeded` is all "put it back" needs; the open question is the gesture.
