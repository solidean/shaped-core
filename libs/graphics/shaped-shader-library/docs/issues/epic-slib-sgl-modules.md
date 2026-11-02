---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/shader_library.cc
  - symbol: slib::shader_library::read_modules
---

# How slib reads, caches and watches SGL modules

An SGL source's `use` reaches the modules of the library's module directories, which slib reads as files, like an include.
The first cut reads, keys and watches them coarsely; each child is one place that shows.
