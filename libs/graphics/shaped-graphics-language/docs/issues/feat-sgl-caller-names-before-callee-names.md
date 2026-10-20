---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/flatten.cc
  - symbol: name_mint
---

# An inlined callee's names are minted before the caller's own

In the helpers sample's `main_ps`, the parameter `light` of the inlined `banded(light, ...)` keeps its name, and the program's own `let light = banded(...)` becomes `light_1`.
The names a reader wrote in the entry point are the ones the text should keep, so they could be reserved first, or the inlined ones minted second.
