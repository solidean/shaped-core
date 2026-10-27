---
created: 2026-10-17
parent: epic-sg-raster-gaps
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/command_list
  - symbol: dispatch_groups_indirect
---

# Indirect draws

`draw_indirect` and count buffers.
Indirect *dispatch* landed as `cmd.compute.dispatch_groups_indirect`.
Its args validation, its `indirect_read` declaration and its aliasing rule are the precedent to follow.
