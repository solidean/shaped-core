---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_expr.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/literal-types.md
---

# Literal folding

`1 / 3` over integer literals alone is refused (CHK-313), and so is an operator over them that only a float takes (CHK-257).
Folding literal subtrees is what [literal-types.md](../spec/incubator/literal-types.md) sketches in their place.
