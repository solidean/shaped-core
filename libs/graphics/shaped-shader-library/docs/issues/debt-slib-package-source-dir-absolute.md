---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-shader-library/cmake/generate_shader_package.py
  - file: libs/graphics/shaped-shader-library/src/shaped-shader-library/shader_library.cc
---

# A shader package's generated source carries its absolute source directory for hot reload

`generate_shader_package.py` writes each package's `source_dir` and its module directories into the generated `.cc` as absolute paths.
Hot reload mounts a `real_filesystem` over them where shaders are not frozen.
So every shader package's generated source differs between two checkouts, as does each library linking one, and the `abspaths` gate allows them until this closes.

Wanted: the directories as paths relative to the top-level source directory, joined at run time onto a root the application gives the shader library.
A test or an example would give `nx::source_root()`; an application that gives none runs on the embedded files without hot reload, as a frozen build does.
The shipped library itself then never needs a source root.
