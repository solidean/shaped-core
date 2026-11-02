---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check.cc
  - symbol: checker::compile
---

# Features: what granting and counting them cannot do yet

A builtin's call counts its feature where the entry point reaches it (CHK-322), so a body uses features now.
The children are the places that count stops short: a grant scoped to a block, a use through another symbol, and a hot reload that needs a new feature.
