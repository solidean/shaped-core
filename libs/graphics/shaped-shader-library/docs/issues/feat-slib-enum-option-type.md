---
created: 2026-10-17
parent: epic-slib-sgl-options-in-host-code
refs:
  - file: libs/graphics/shaped-shader-library/cmake/sgl_host_code.py
  - symbol: options_struct
---

# A program's own enum option has no C++ type

`sgl_host_code.py` types `bool`, `int` and `pixel_format` alone.
So the file's options struct leaves such an option out, and a wrapper reaching one is a generator error.

Generating the enum beside the struct, from the cases `sgl describe` would have to report, would close it.
