---
created: 2026-10-17
parent: epic-sg-raytracing-pipeline-gaps
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/raytracing/raytracing_shader_table.hh
---

# Local root signatures

A shader-table record holds only a 32-byte shader identifier today, with no local root arguments.
So every resource binds through the pipeline's global root signature.
