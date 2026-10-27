---
created: 2026-10-17
blocked-by: [feat-slib-named-fragment-asset-kind]
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/material/builtin_material_types.cc
---

# The builtin material fragments are C++ string literals

A material type's `shader` is an SGL fragment, not a compilable shader, so the builtins carry theirs as string literals in `material/builtin_material_types.cc`.
slib has no named-fragment asset kind, so `sc_add_shader_package` has nothing to declare one as.
Moving them under `shaders/` once slib can declare a fragment gets editor support and hot reload.
