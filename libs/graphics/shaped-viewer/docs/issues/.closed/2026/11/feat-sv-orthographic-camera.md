---
created: 2026-11-11
parent: epic-sv-baking
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/rendering/view_renderer.cc
  - symbol: camera_record_of
closed: 2026-11-12
resolution: fixed
---

# An orthographic camera through every path

The viewer's camera is perspective only.
A bake looks straight down on a tile, and a technical view wants parallel lines to stay parallel, so the camera gains an orthographic projection beside the perspective one.

What it touches:

- **Ray generation**: an orthographic primary leaves from a point on the image plane along one direction, where a perspective one leaves the eye.
- **Ray cones**: an orthographic primary's cone starts with a width, the pixel's size in meters, and no spread; `primary_spread` becomes a width there.
  The tracer's `cone_width` already carries one, so the cone math holds.
- **Raster**: the projection matrix, and the depth a pick reads.
- **The camera record** the shaders read (`camera_record_of`), and the orbit controller, which zooms by the view's size and not by a distance.

Test: an orthographic view of a plane of known texels reads the level its texel size predicts, wherever on the image, where a perspective one varies with distance.
