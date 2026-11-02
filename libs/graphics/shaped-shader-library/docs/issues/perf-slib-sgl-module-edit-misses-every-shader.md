---
created: 2026-10-17
parent: epic-slib-sgl-modules
refs:
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/compiler/sgl_compiler.cc
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/impl/sgl_emit_cache.cc
---

# An edit to one SGL module misses the preprocess cache for every SGL shader

The SGL edge's key hashes every module file of the library, used or not.

Keying on the modules a result actually used, stored with their hashes and checked on read, would invalidate only that module's users.
The result already records them as `used_modules`.
