---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/examples/openpbr-spheres.cc
  - file: libs/graphics/shaped-viewer/examples/hello-cube.cc
---

# sv's examples hand-roll their primitive meshes

`openpbr-spheres` builds a UV sphere and `hello-cube` a cube, each by hand.
Primitive meshing belongs in typed-geometry rather than in an example, and two callers is enough to want it.
A `tg::` sphere / box tessellation is the library extension that replaces both.
