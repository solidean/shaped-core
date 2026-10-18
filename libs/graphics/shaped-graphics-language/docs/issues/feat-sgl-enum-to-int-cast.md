---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/tests/check/enum-check-test.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/enum-futures.md
---

# An enum does not convert to `int`

`h.kind as int` is refused (CHK-150), so storing a `hit_kind` in a buffer takes a `case`.
[enum-futures.md](../spec/incubator/enum-futures.md) holds the cast.
