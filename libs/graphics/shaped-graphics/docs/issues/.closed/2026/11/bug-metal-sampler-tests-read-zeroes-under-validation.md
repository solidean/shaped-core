---
created: 2026-10-27
tags: [metal]
refs:
  - file: libs/graphics/shaped-graphics/tests/pipeline/sampler-test.cc
closed: 2026-11-02
resolution: obsolete
---

# The sampler tests read zeroes on metal under shader validation

Both tests in tests/pipeline/sampler-test.cc pass on metal without validation, and read zeroes with MTL_SHADER_VALIDATION=1.
The metal binary itself passes under GPU validation; these two are the only failures in shaped-graphics-test.
Whether it is our binding of the sampler or the validation layer's is not yet known.

On 2026-11-02 both tests, and all of shaped-graphics-test, passed with MTL_SHADER_VALIDATION=1 on an M4 under macOS 27.2, at 56204e8d8 as well as later.
Whichever change fixed it, it no longer reproduces.
