---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/legalize/legalize_cases.cc
---

# A `case` on a `bool` is a copy and `== true`

slug_quads' `main_ps`, from `case even_odd:` in `modules/slug.sgl`, is `let case_value: bool = even_odd; if case_value == true { ... } else { ... }` in WGSL.
The chain form of C1 could leave a scrutinee that is a local unbound, and test a bool pattern as `if x` or `if !x`.
