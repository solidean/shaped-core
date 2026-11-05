---
created: 2026-11-05
parent: epic-sgl-robustness-profiles
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/builtins/registry.hh
---

# Guard the maths builtins that have no value for some arguments

The registry marks each builtin that may have no value with `undefined_when`.
Those are `clamp` with low above high, `pow`, `asin` and `acos` outside -1 to 1, `atan2(0, 0)`, `smoothstep` with low not below high, and the matrix part builtins.
LEGAL-65 guards integer division alone, found by its name; each of these needs its test written in the target's terms.
A registry field holding the guard's condition would let one walk serve them all.
