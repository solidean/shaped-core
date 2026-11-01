---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_resources.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_generics.cc
---

# Values as type arguments

`image_2d[.rgba8_unorm]` takes an enum case, and the checker reads exactly that argument today, as a special case of image types (CHK-200).

The general feature exists now for `int`, `uint`, `bool` and `float`: a value parameter `[n: int]` of a function or a generic struct (CHK-338, CHK-339).
A call states one, `blur[3](c)`, a type position names one, `ring[4, float]`, and an array's length deduces one, `items: T[n]`.

What is left:

* An enum value parameter, `[f: format]`, which `image_2d[.rgba8_unorm]` would become an ordinary use of.
  The const evaluator's leaf for an enum case is the open question.
* A `float` or `bool` constant in a type position, where only an `int` is read today.
