---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_resources.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_generics.cc
closed: 2026-11-01
resolution: fixed
---

# Values as type arguments

`image_2d[.rgba8_unorm]` takes an enum case, and the checker reads exactly that argument today, as a special case of image types (CHK-200).

The general feature exists now for `int`, `uint`, `bool`, `float` and enums: a value parameter `[n: int]` or `[m: mode]` of a function or a generic struct (CHK-338, CHK-339).
A call states one, `blur[3](../../../c)`, a type position names one, `ring[4, float]` or `op[.add]`, and an array's length deduces one, `items: T[n]`.

What is left:

* An image is a resource with slots, so it stays a builtin type rather than a generic struct.
  What a generic wants of it exists: `image_2d[f]` over a `pixel_format` value parameter, deduced from the image handed over (CHK-200).
  A texture's element and an acceleration structure's geometry kind could take value parameters the same way, once a program asks.
* A `float` or `bool` constant in a type position, where only an `int` and an enum case are read today.
