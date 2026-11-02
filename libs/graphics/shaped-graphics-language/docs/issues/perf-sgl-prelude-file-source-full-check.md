---
created: 2026-10-17
parent: epic-sgl-prelude-check-cost
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/driver/impl/front_end.cc
  - symbol: sgl::prelude_file_of
---

# A source that stands in for a prelude file checks the whole prelude

Such a source is checked as that file of the prelude (`prelude_file_of`).
It is `prelude/core.sgl` open in an editor, or a user's `shaders/prelude/core.sgl`.
Such a check has no checked prelude to continue, so it takes the full pass, which is the 50 ms a compile took before.
