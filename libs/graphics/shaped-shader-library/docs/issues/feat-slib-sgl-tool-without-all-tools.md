---
created: 2026-10-15
refs:
  - file: libs/graphics/shaped-shader-library/cmake/ShaderPackage.cmake
  - symbol: sc_add_shader_package
---

# Building `sgl` for an SGL package takes `SC_BUILD_TOOLS=ON`, which builds every tool

`sc_add_shader_package` runs the `sgl` tool at build time.
A consumer either points `SC_SGL_TOOL` at a prebuilt one, or turns on `SC_BUILD_TOOLS`, which also builds the linter and the language server.
A consumer writing HLSL, WGSL or MSL alone needs neither; one writing SGL could build `sgl` alone, behind a narrower option.
