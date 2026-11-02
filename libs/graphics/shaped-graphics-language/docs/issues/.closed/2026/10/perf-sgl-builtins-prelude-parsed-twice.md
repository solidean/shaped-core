---
created: 2026-10-17
parent: epic-sgl-prelude-check-cost
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/registry.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/driver/prelude.cc
  - symbol: registry::finalize
  - symbol: sgl::parsed_prelude
  - test: sgl profile - one compile of a file, sampled
closed: 2026-10-20
resolution: fixed
---

# A process parses the generated `builtins.sgl` twice

`registry::finalize` parses `prelude_text()` to read each record's names back, and `parsed_prelude()` parses the same text again.
Checking the prelude takes 42 ms on `relwithdebinfo-clang` and 660 ms on `debug-clang`, once per process, and the second parse is about a quarter of the debug one.
`sgl profile - one compile of a file, sampled` times that check apart from the compiles.
