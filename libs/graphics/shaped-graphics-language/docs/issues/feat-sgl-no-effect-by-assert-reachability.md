---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_stmt.cc
---

# `no-effect` in a test, by what can reach an assert

A line of a test without an effect is still code under test when it can reach an `assert`, since it may trigger one.
So CHK-225's warning is too coarse there, and a `void` line is exempt from it today.
The precise rule wants the check pass to know more about the effects of an expression and of a function, "could reach an assert" among them.
A line of a test is then `no-effect` exactly when it has no effect and cannot reach an assert.
