---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/raytracing.sgl
---

# `prelude/raytracing.sgl` repeats one stack walk in its three emulated traversals

The file is ~1200 lines.
Triangles, boxes and both differ only in what a BLAS leaf does.
One walk taking the leaf as a function value (CHK-317) would carry all three, once the inliner's output of it is as tight as the copies are.
