---
created: 2026-10-17
parent: epic-sg-raytracing-pipeline-gaps
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_command_list.cc
---

# metal traces one TLAS per dispatch

A kernel finds its closest hits through the one TLAS's hit-group offsets that `dispatch_rays` binds.
It binds the first bound TLAS's offsets, and logs a warning when the bound groups hold more than one.
Lifting it means an offsets buffer per bound TLAS, and a way for the kernel to know which one it traced.
