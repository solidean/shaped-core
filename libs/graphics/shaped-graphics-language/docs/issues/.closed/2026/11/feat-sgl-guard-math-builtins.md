---
created: 2026-11-05
parent: epic-sgl-robustness-profiles
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/registry.hh
closed: 2026-11-05
resolution: fixed
---

# Guard the maths builtins that have no value for some arguments

The registry marks each builtin that may have no value with `undefined_when`.
Those are `clamp` with low above high, `pow`, `asin` and `acos` outside -1 to 1, `atan2(0, 0)`, `smoothstep` with low not below high, and the matrix part builtins.
Done on 2026-11-05: `pow`, `asin`, `acos`, `atan2`, `smoothstep` and `clamp`, each tested as the interpreter tests it, so a NaN behaves alike.
A deferring guard hands each a safe operand: a base of 1, an argument of 0, an `x` of 1, edges of 0 and 1, a high bound equal to the low one.
Also done: a matrix's `row` and `column` outside the matrix, and an overload whose operands differ in shape, a scalar read as every lane of the vector beside it.
A `clamp` whose bounds differ in shape takes no safe operand before a barrier, so it stays unguarded there.
