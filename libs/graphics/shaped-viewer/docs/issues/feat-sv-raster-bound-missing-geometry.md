---
created: 2026-11-09
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/raster_bound_routine.cc
parent: epic-sv-raster-bound
---

# raster_bound draws no frame meshes over their own index buffers

`sv::raster_bound_routine` skips one kind of draw the bindless raster mode makes: a frame mesh drawn over its own index buffer, whose triangles the pixel stage would look up in the pool.
A scene with one draws without it, silently.
It wants its indices in the pool, or a pulled path over its own buffer.

Decals draw since 2026-11-11: their records and shapes are lanes of group 0's constant buffer, 1024 of them, so they spend no storage buffer.
A layer holding more paints the decals that fit and says so once.

Quadric batches draw since 2026-11-11: their records and boxes are blocks of the mesh manager's vertex pool on every backend.
So the bound mode reaches them through `bound.vertices`, and its traced shadows, indirect light and reflections intersect their procedural boxes.
They cast no shadow-map shadow yet: the caster pass draws triangles alone.
