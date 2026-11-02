---
created: 2026-10-17
parent: epic-sv-drawings-and-decals
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/drawing/decal.hh
---

# A decal's footprint past a bounce is too small

It is the camera's pixel at the segment's own length, exact for a primary ray.
A reflected ray would carry a ray cone, which the tracer does not yet.
Its jitter antialiases the edges there instead, slowly.
