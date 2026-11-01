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

- **The natural rule (EMIT-110) decides the defaults**: an array strides by its element rounded up to its alignment, so a `float3[N]` strides by 16, and a `mat3` is three 3-vectors, 48 bytes.
- **`@padding` names the cases where that disagrees with a C++ struct**, and is already spelled (CHK-369):
  `.none` gives a `mat3` its 36 bytes of `tg::mat3f` and a `float3[N]` its 12-byte stride, the latter in a buffer only.
  `.every_column` names the 48-byte `mat3`, and `.every_element` starts each element of a block's array on a row.
- **An array of elements shorter than a row in a constant block is refused without `.every_element`**, since a runtime index into it needs component arithmetic.
  WGSL reads such an array through `array<vec4f, N>`, and the host needs a padded element type like `slib::gpu_padded`.
- **The mechanism that is missing** is a memory form holding an array, which a runtime index walks into.
- **WGSL passes no matrix and no array between stages**, so both cross as one location per column or element.

**Arrays in buffer elements and structs have landed** (CHK-291, EMIT-169, EMIT-170): at their natural stride, in a block where they take whole rows.
A block's sub-row array has landed behind `@padding(.every_element)` (EMIT-172).
Arrays of `mat3` have landed (EMIT-168), and multi-dimensional arrays whose innermost element HLSL need not widen (EMIT-171).
`mat2` in memory has landed (EMIT-175), and with it MSL's placement of a constant block's structs (EMIT-176).
What waits is the rest of EMIT-171 (packed arrays, and multi-dimensional ones of 3-vectors or `mat3`), a packed `mat2`, and `.every_column`.

**Matrices and arrays across a stage edge have landed** (CHK-422, EMIT-174): a location per column or element, between any two shading stages and from a vertex buffer.
A pixel output takes neither, since each of its locations is a render target.

**`mat3` in GPU memory has landed** (CHK-397, EMIT-168): 48 bytes natural, 36 under `@padding(.none)`, and HLSL holds it as a `float4x3`.

**A constant block's array of whole rows has landed** (EMIT-164): `float4`, `int4`, `uint4` and `mat4` elements, which every target places alike.
It was the gap real shaders hit most, 8 of the first 104 bgfx translations in shader-rosetta, and none of them is blocked by it now.
What remains is every other array in GPU memory: padded elements in a block, arrays in a struct or a buffer's element, and arrays across a stage edge.
