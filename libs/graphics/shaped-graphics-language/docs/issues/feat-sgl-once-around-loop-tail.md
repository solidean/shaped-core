---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/legalize/legalize_exits.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/legalization.md
---

# A once around a loop and the tail behind it could be an `if` on the flag

The cube and the helpers sample write `do { for (...) { ...; flag = true; break; } if (flag) break; result = ...; } while (false)`.
Where all that follows the flag-tested loop is the tail, the `once` and the second test could go: `for (...) { ... } if (!flag) { result = ...; }`.
It is close to the open question of `legalization.md`, whether X2 should write statements twice.
