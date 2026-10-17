---
created: 2026-10-17
parent: epic-sgl-modules-beyond-a-tracer
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check.cc
  - file: libs/graphics/shaped-graphics-language/tests/corpus/modules
---

# What a module the program uses cannot hold yet

CHK-350 refuses three things in a module the program uses: a file-scope sampler, an `@operator` function, and a second binding of one name in one list.
Its entry points, pipelines and tests are built only when it is compiled as a program.
