---
created: 2026-10-17
refs:
  - symbol: sg::dispatch_args
  - symbol: dispatch_groups_indirect
---

# Every indirect dispatch re-types the shader's count struct by hand

A shader writes its group counts as its own `uint3` struct, and each call site spells `reinterpret_as<sg::dispatch_args>()` to hand it to `dispatch_groups_indirect`.
A prelude `dispatch_args` type in SGL that generates as `sg::dispatch_args` would make the buffer the right type from the start, and remove every one of those casts.
There are eight such casts in the tree today.
