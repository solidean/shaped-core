---
created: 2026-10-27
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics/tests/pipeline/sampler-test.cc
---

# The sampler tests read zeroes on metal under shader validation

Both tests in tests/pipeline/sampler-test.cc pass on metal without validation, and read zeroes with MTL_SHADER_VALIDATION=1.
The metal binary itself passes under GPU validation; these two are the only failures in shaped-graphics-test.
Whether it is our binding of the sampler or the validation layer's is not yet known.
