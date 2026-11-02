---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/builtin_material_types.cc
---

# slib has no named-fragment asset kind

An SGL fragment is not a compilable shader, so `sc_add_shader_package` has nothing to declare one as.
shaped-viewer's material types are such fragments, and its builtins therefore live as string literals in C++ (`debt-sv-builtin-material-fragments-as-strings`).
Declaring a fragment in a package would give them editor support and hot reload.
