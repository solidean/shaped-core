---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/driver/prelude.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check.cc
  - test: sgl profile - *
---

# What a compile and a process pay for the prelude

The prelude is checked once per process, and every compile then continues from that checked state.
Both halves still carry work that is not the program's own.
The children name each cut, with what it costs today and the test that measures it.

The persistent shader cache of slib does not skip the prelude check, so this cost is paid once per process wherever a pipeline is acquired (shaped-shader-library's `docs/structure.md`).
