---
created: 2026-10-17
refs:
  - symbol: slib::compile_hit_group
---

# The SGL front end runs once per entry point of a hit group, the first time

`slib::compile_hit_group` compiles each stage from the source, and a cutout group has three, so the parse and the check run three times.
slib caches each in memory and across runs, so a repeat costs a lookup.
A group compiled for the first time still pays all three.
Found from shaped-viewer, whose generated material hit groups are the caller.
