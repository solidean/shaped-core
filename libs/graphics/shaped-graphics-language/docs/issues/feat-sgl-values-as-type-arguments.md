---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_resources.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_generics.cc
---

# Values as type arguments

`image_2d[.rgba8_unorm]` takes an enum case, and the checker reads exactly that argument today, as a special case of image types (CHK-200).

The general feature exists now for `int`, `uint`, `bool`, `float` and enums: a value parameter `[n: int]` or `[m: mode]` of a function or a generic struct (CHK-338, CHK-339).
A call states one, `blur[3](c)`, a type position names one, `ring[4, float]` or `op[.add]`, and an array's length deduces one, `items: T[n]`.

What is left:

* `image_2d[.rgba8_unorm]` as an ordinary generic struct over a `format` value parameter, rather than the builtin type the registry declares.
* A `float` or `bool` constant in a type position, where only an `int` and an enum case are read today.
