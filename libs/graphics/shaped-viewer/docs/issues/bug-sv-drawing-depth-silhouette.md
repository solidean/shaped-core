---
created: 2026-10-17
parent: epic-sv-drawings-and-decals
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/depth_fill_routine.cc
  - file: libs/graphics/shaped-viewer/shaders/depth_fill.sgl
---

# A 3D drawing's edge shimmers along a silhouette

**A 3D drawing is occluded by the depth of the trace's first sample, reprojected to the pixel center.**
The plane taken is the hit triangle's geometric normal's.
It is exact where the pixel center lands on the same triangle, and a sub-pixel step off on a neighboring triangle of a curved mesh or on a quadric.

A silhouette pixel holds one surface's depth, so a drawing's edge along a silhouette is hard rather than antialiased.
That depth is taken again from a jittered sample each accumulation frame, so at a silhouette it flips until the image converges.
A 3D drawing's edge shimmers there, and an annotation anchored on a silhouette pixel can toggle between filled and hollow.
