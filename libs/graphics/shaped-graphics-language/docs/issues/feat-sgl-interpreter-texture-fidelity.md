---
created: 2026-11-06
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/interpret/textures.cc
  - issue: feat-sgl-test-texture-bindings
---

# What the interpreter's textures still leave out

What the interpreter's textures still leave out, each a place where a test's run and a GPU's can part (EVAL-120, EVAL-126).

* A format's quantization of what an image stores: a GPU rounds a value it stores to an `rgba8_unorm` texel, and the run keeps it exact.
* Anisotropic filtering, which the run leaves out, and filtering across a cube face's edge into the next face.
* A test that states the texels of an `@atomic` image: `test_texel` takes no such image (CHK-373), so its texels come from a driver alone.
