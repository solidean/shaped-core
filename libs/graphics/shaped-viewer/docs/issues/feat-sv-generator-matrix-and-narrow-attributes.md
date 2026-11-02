---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/shader_generator.cc
  - symbol: sv::sgl_type_of
  - symbol: sv::generate_material_shader
---

# The material generator handles scalars and vectors of f32 / i32 / u32 only

A matrix attribute has no settled raw-buffer layout here, and the narrow and 64-bit scalars need 16-bit types or a split load.
`sgl_type_of` returns empty for those and `generate_material_shader` asserts, rather than emitting something that will not compile.
