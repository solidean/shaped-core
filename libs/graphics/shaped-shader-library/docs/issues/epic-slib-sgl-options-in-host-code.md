---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-shader-library/cmake/sgl_host_code.py
---

# SGL options the generated host code cannot type yet

`sgl_host_code.py` generates C++ types for an SGL file's groups, blocks and options, and refuses the cases where an option would change a generated type.
Each child is one such refusal and what would close it.
