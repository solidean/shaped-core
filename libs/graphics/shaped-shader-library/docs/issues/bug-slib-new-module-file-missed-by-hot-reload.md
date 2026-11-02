---
created: 2026-10-17
parent: epic-slib-sgl-modules
refs:
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/shader_library.cc
---

# A module file that appears is not seen by hot reload

A shader depends on the module files it reached, or on every one that existed when it failed.
So a file created later is nobody's dependency.

The way out is a dependency per module directory whose revision is its listing.
