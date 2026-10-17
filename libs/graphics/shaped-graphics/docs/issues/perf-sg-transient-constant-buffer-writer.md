---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/binding/binding_group.hh
  - symbol: create_binding_group
---

# A group's implicit constant buffer is one upload each

`create_binding_group` allocates a buffer for a generated group's plain members and fills it with one copy per group.
On `ctx.transient` that copy is recorded inline into the `cmd` the call takes, and on `ctx.persistent` it goes through `ctx.upload`.
A per-frame group wants a transient constant-buffer writer instead: a ring in host-visible device memory (ReBAR where there is some), suballocated per epoch and written in place.
