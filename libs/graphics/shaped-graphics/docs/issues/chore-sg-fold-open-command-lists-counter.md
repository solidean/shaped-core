---
created: 2026-10-17
tags: [dx12]
refs:
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_context.hh
  - file: libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc
---

# Fold the redundant `_open_command_lists` counter into the slot allocator's live count

The epoch-advance counter `_open_command_lists` is bumped per create and dropped per submit or drop.
The slot allocator already keeps a live count that answers the same question.
