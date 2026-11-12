---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/interpret/interpret.hh
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/test/run_tests.cc
closed: 2026-11-06
resolution: fixed
---

# Textures, images and samplers in a test's bindings

A test lists values, buffers and acceleration structures, and its driver binds them (CHK-333, EVAL-94).
A binding that holds a texture, an image or a sampler is `unsupported-yet` in a test, since the interpreter has no texel to read yet.
It needs a texture a driver can bind, `member_data` taking its texels, and the texture builtins' evaluators reading them.

## Done on 2026-11-05

A driver binds a texture's or an image's texels through `member_data::textures`, and a dynamic sampler's state through `member_data::samplers` (EVAL-94, EVAL-120).
The interpreter evaluates every texture builtin on them: loads, image loads and stores, the size, level, layer and sample queries, and samples (EVAL-121 to EVAL-126).
Sampling covers nearest and linear filtering, the three address modes, mip levels from an explicit level or gradients, comparison samplers, gathers, and cubes.
An image store comes back to the driver's `mutable_texels`, and the outcome compares stored images as it does buffers.
CHK-333 and CHK-314 no longer refuse a texture, an image or a sampler in a test.

## Done on 2026-11-06

A test states a texel itself with `t.test_texel(xy, value)`, which grows a texture its driver bound nothing to, and a dynamic sampler's state with `s.test_state(from)` (EVAL-127, EVAL-128).
So `sgl test` and a corpus file bind a texture with no C++ driver; `tests/corpus/tests/texels.sgl` is the corpus.
A texel atomic of an `@atomic` image updates the texel the run holds, as a buffer's atomic does its element (EVAL-100).
What the textures still leave out is [feat-sgl-interpreter-texture-fidelity](../../../feat-sgl-interpreter-texture-fidelity.md).
