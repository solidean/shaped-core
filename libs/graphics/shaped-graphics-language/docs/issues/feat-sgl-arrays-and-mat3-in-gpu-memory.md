---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check_decl.cc
  - file: libs/graphics/shaped-graphics-language/tests/check/array-check-test.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
---

# Arrays and `mat3` in GPU memory, and matrices and arrays across a stage edge

An array is a value everywhere else (CHK-285).
It is `unsupported-yet` wherever GPU memory or a stage edge holds one (CHK-291).
`mat3` is a value of function bodies alone, refused wherever memory or a stage edge would hold one (CHK-397).
Their layout is decided (the spec's emitting file, "Open"), and they land together, after the tier-1 work.

- **In a constant block an array element starts a row**: `slib::row<T>` pads a shorter one to 16 bytes on the host, and WGSL reads it through `array<vec4f, N>`.
- **The mechanism that is missing** is a memory form holding an array, which a runtime index walks into.
- **A `mat3` is its three columns, each starting a row, in a block** (44 bytes), and `slib::gpu_mat3` holds that on the host.
  In a buffer's element it is 36 bytes, which is `tg::mat3f` itself.
- **WGSL passes no matrix and no array between stages**, so both cross as one location per column or element.

**A constant block's array of whole rows has landed** (EMIT-164): `float4`, `int4`, `uint4` and `mat4` elements, which every target places alike.
It was the gap real shaders hit most, 8 of the first 104 bgfx translations in shader-rosetta, and none of them is blocked by it now.
What remains is every other array in GPU memory: padded elements in a block, arrays in a struct or a buffer's element, and arrays across a stage edge.
