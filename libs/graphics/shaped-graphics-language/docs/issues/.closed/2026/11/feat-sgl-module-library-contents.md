---
created: 2026-10-17
parent: epic-sgl-modules-beyond-a-tracer
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check.cc
  - file: libs/graphics/shaped-graphics-language/tests/corpus/modules
closed: 2026-11-02
resolution: fixed
---

# What a module the program uses cannot hold yet

CHK-350 refuses a file-scope sampler in a module the program uses.
Its operators apply where it is used, and one list may name two bindings of one name from two modules.
Its entry points, pipelines and tests are built only when it is compiled as a program.
