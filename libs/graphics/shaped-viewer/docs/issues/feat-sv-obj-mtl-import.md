---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/asset/impl/obj_import.cc
---

# OBJ imports carry material names only, without `.mtl`

`.mtl` is referenced by OBJ and planned in babel.
Until babel reads it, an OBJ import carries geometry plus material NAMES only, and every name becomes the same unbound `openpbr` material.
