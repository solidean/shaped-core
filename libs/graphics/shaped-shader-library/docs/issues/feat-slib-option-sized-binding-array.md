---
created: 2026-10-17
parent: epic-slib-sgl-options-in-host-code
refs:
  - file: libs/graphics/shaped-shader-library/cmake/sgl_host_code.py
  - symbol: refuse_options
---

# A binding array whose length names an SGL option has no generated group

The group's field is a `cc::fixed_array` of that length, so the type would change with the value.
`sgl_host_code.py` refuses it, and the file's entry points are listed alone.

A field of a run-time length, `cc::vector` or a span, with `declared_bindings(values)` setting the binding's count, would close it.
