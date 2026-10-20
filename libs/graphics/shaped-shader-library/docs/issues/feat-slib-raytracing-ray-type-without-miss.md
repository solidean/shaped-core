---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-shader-library/cmake/sgl_host_code.py
---

# An SGL `@raytracing pipeline` cannot leave a ray type without a miss

`sgl_host_code.py` refuses such a pipeline, since slib cannot table it.
A ray type without a miss wants an empty miss record, which sg's shader table has no spelling for yet.
