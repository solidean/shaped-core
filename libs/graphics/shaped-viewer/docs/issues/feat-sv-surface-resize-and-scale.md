---
created: 2026-11-09
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/viewer.cc
parent: epic-sv-raster-bound
---

# A canvas surface follows no resize and lays drawings out at scale 1.0

`viewer::set_surface_size` exists, but graphics/web-viewer's page never calls it: its canvas stays 1280x720 whatever the page's layout does.
And with no window there is no display scale, so drawings and imgui lay out at 1.0 on a high-density screen.

Wanted: the page follows its canvas's size (a ResizeObserver feeding `set_surface_size`, the canvas sized by `devicePixelRatio`), and the viewer takes a display scale for a surface.
