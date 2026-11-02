---
created: 2026-10-17
parent: epic-slib-sgl-modules
refs:
  - file: libs/graphics/shaped-shader-library/cmake/ShaderPackage.cmake
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/shader_library.cc
---

# Module names are the library's at run time and the package's at build time

A build describes a package against its own module directories, while a running library merges every package's.

So two packages whose directories each declare a module `common` build cleanly and then become one module `common` at run time.
And a package can `use` a module only another package's directory holds without listing it, which works only where both are added.

Neither is checked yet; separating the two cleanly needs a design of its own.
