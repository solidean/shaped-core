---
created: 2026-10-17
parent: epic-sgl-prelude-check-cost
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/flatten.cc
  - symbol: flatten_entry_point
  - symbol: name_mint
  - test: sgl profile - one compile of a file, sampled
---

# Every flat tree takes every module-level name into a mint of its own

`flatten_entry_point` reserves each symbol's name, the prelude's thousands included.
It is a tenth of a compile of the sgl-cube's vertex shader on `debug-clang`, which `sgl profile - one compile of a file, sampled` measures.
The emitter's mint already goes on from the tree's (`name_mint::parent`).
The tree's could go on from one set of the module's names, given an owner that outlives every tree.
