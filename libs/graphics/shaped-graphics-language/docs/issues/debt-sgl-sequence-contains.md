---
created: 2026-10-27
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/checked_module.hh
  - symbol: sgl::check::holds
---

# SGL hand-rolls whether a sequence holds a value

The per-entry-point diagnostics keep sets of entry points as `cc::vector<symbol_id>`, short enough that a map is no better, and ask of them whether one is in.
`check::holds` in `check/checked_module.hh` is that loop.
It stands in for `contains(x)` on `cc::vector` and `cc::span`, which `vector.hh` already lists among its TODOs beside `find` and `count`.
