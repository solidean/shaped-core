---
created: 2026-10-22
refs:
  - file: libs/graphics/shaped-rendering/shaders/compression/bc.sgl
---

# A vector cannot be subscripted by a value known only at run time

`v[i]` on a vector is `unsupported-yet: a subscript on anything but a buffer, an image or an array`.
HLSL, WGSL and MSL all take a vector subscript, and a shader that picks a lane by a loop index needs one.
So `bc.sgl` writes `fun channel(v: int4, c: int)` as a chain of four `if`s, which a reader of HLSL would write as `v[c]`.
Where the index is a constant after unrolling the target folds the chain, and everywhere else it is the branches the source wrote.
It breaks the readme's aspiration: code written the same way in both languages, which SGL cannot write.
