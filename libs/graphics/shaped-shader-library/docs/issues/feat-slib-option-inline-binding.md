---
created: 2026-10-17
parent: epic-slib-sgl-options-in-host-code
refs:
  - file: libs/graphics/shaped-shader-library/cmake/sgl_host_code.py
  - symbol: refuse_options
---

# An `@inline` binding that names an SGL option has no generated type

Its type is the block's bytes at the defaults, so a value that moved them would leave the host writing the wrong layout.
`sgl_host_code.py` refuses it.

A block laid out per set of values, as a group's layout already is, would close it.
