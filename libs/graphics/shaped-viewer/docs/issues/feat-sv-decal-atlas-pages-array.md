---
created: 2026-10-17
parent: epic-sv-drawings-and-decals
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/drawing/decal.hh
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/drawing/drawing_manager.cc
---

# Every decal lives in one atlas, so a frame's decals share 512 rows

A trace binds one `slug.tables`, so the frame's decals have to fit its 512 rows together.
Binding the atlas pages as an array would lift that.
The TODO this came from waited for SGL to have arrays of bindings; module `tracer`'s `binding bindless` now uses them (`require binding_arrays`), so check whether that blocker is gone.
