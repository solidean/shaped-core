---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/shaders/sgl/tracer_bindings.sgl
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/resources/bindless_tables.hh
---

# The bindless table budgets are the module's, not a config

`binding bindless` in shaders/sgl/tracer_bindings.sgl is the one declaration, and the manager lays its group out from it.
So a table's size is an edit to that file.
A per-viewer budget would need the module to take its counts as options, which SGL's `require` and option machinery does not reach for binding arrays yet.
