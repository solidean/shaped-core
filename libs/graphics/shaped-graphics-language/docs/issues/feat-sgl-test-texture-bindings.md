---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/interpret/interpret.hh
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/test/run_tests.cc
---

# Textures, images and samplers in a test's bindings

A test lists values, buffers and acceleration structures, and its driver binds them (CHK-333, EVAL-94).
A binding that holds a texture, an image or a sampler is `unsupported-yet` in a test, since the interpreter has no texel to read yet.
It needs a texture a driver can bind, `member_data` taking its texels, and the texture builtins' evaluators reading them.
