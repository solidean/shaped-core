---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/docs/coding-guidelines.md
---

# A blessed escape hatch to the raw GPU handles

An sg API returning the raw underlying GPU handles without exposing the concrete backend types.
Then a caller never reaches for `dynamic_cast` to an `sg::backend::*` type.
See the [coding-guidelines](../coding-guidelines.md) escape-hatch note.
