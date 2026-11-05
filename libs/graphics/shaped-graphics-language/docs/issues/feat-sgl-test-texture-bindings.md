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

## Done on 2026-11-05

A driver binds a texture's or an image's texels through `member_data::textures`, and a dynamic sampler's state through `member_data::samplers` (EVAL-94, EVAL-120).
The interpreter evaluates every texture builtin on them: loads, image loads and stores, the size, level, layer and sample queries, and samples (EVAL-121 to EVAL-126).
Sampling covers nearest and linear filtering, the three address modes, mip levels from an explicit level or gradients, comparison samplers, gathers, and cubes.
An image store comes back to the driver's `mutable_texels`, and the outcome compares stored images as it does buffers.
CHK-333 and CHK-314 no longer refuse a texture, an image or a sampler in a test.

## Left

* A way for a test to state texels in SGL, so `sgl test` and a corpus file can bind a texture without a C++ driver; today such a texture is empty there.
* Texel atomics of an `@atomic` image, which still read zero (EVAL-100).
* A format's quantization of what an image stores, which the run keeps exact (EVAL-120).
* Anisotropic filtering, which the run leaves out, and filtering across a cube face's edge (EVAL-126).
