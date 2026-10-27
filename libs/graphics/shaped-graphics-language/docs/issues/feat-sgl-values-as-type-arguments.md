---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_resources.cc
---

# Values as type arguments

`image_2d[.rgba8_unorm]` takes an enum case, and the checker reads exactly that argument today, as a special case of image types (CHK-200).
The general feature is a type parameterized on an integer or an enum value, which math templated on a dimension wants as well.
It also lets code branch on the value.
