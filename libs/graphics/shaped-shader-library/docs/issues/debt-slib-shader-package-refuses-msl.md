---
created: 2026-11-09
tags: [metal]
refs:
  - file: libs/graphics/shaped-shader-library/cmake/generate_shader_package.py:75
  - file: libs/graphics/shaped-rendering/tests/bench/sgl-against-msl-benchmark.cc
claimed-by: Claude (for Julius Nehring-Wirxel)
claimed-at: 2026-11-10T12:29Z
---

# sc_add_shader_package refuses an MSL package, which the docs say exists

shaped-core's CLAUDE.md says a package is written in HLSL, WGSL, MSL or SGL, and slib has `shader_language::metal`.
But `generate_shader_package.py` takes `VALID_LANGUAGES = ("hlsl", "wgsl", "sgl")`, so `LANGUAGE msl` fails at configure.
So `sgl-against-msl-benchmark.cc` reads its `.metal` files from the source tree and compiles them through `shader_fixtures().compile_source` instead of a package.
Either the generator learns `msl`, with typed symbols as for WGSL, or the docs stop promising it.
