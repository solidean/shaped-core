---
created: 2026-10-17
parent: epic-slib-sgl-modules
refs:
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/shader_library.cc
  - symbol: slib::shader_library::read_modules
---

# SGL modules are read whole on every compile

Every SGL compile lists and reads every module directory of the library.
