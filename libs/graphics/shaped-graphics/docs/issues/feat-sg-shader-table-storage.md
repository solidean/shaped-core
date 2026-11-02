---
created: 2026-10-17
parent: epic-sg-raytracing-pipeline-gaps
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/types.hh
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_raytracing_shader_table.cc
  - symbol: sg::raytracing_shader_table
---

# A dedicated buffer for the shader table

`raytracing_shader_table` exists, but its records sit in a plain shader-readable, copy-dst buffer as a stand-in.
[types.hh](../../src/shaped-graphics/types.hh) rules an SBT out of `buffer_usage` deliberately, so the storage needs a type of its own.
