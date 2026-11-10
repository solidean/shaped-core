---
created: 2026-11-09
tags: [metal]
refs:
  - file: libs/graphics/shaped-shader-library/cmake/generate_shader_package.py:75
  - file: libs/graphics/shaped-rendering/tests/bench/sgl-against-msl-benchmark.cc
closed: 2026-11-10
resolution: fixed
---

# sc_add_shader_package refuses an MSL package, which the docs say exists

shaped-core's CLAUDE.md says a package is written in HLSL, WGSL, MSL or SGL, and slib has `shader_language::metal`.
But `generate_shader_package.py` takes `VALID_LANGUAGES = ("hlsl", "wgsl", "sgl")`, so `LANGUAGE msl` fails at configure.
So `sgl-against-msl-benchmark.cc` reads its `.metal` files from the source tree and compiles them through `shader_fixtures().compile_source` instead of a package.
Either the generator learns `msl`, with typed symbols as for WGSL, or the docs stop promising it.

## Resolved

The generator takes `LANGUAGE msl` now, as WGSL is taken: each `.metal` file is one object with its entry points under their stage, reflected at runtime by slib's metal compiler.
The package's language is `slib::shader_language::metal`, the enumerator the compiler already reported.
`slib metal compiler - an MSL package's shader compiles through the library and is reflected` pins it in slib's own tests, on Apple.
sr's `sgl-against-msl-benchmark.cc` acquires its MSL from package `sr_bench_msl_shaders` instead of reading the source tree.
