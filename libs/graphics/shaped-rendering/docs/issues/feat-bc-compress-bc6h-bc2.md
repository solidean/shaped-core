---
created: 2026-10-17
refs:
  - symbol: sr::bc_compress_routine
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/compression/bc_compress_routine.hh
  - file: libs/graphics/shaped-rendering/shaders/compression/bc.sgl
  - test: sr - BC*
---

# BC6H and BC2 encoders for `sr::bc_compress_routine`

BC6H, the HDR block format, has no encoder, and neither has BC2.
`sr::bc_compress_routine` covers BC1, BC3, BC4, BC5 and BC7, the signed BC4 and BC5 included.
A float source is clamped to 0..1 today, so HDR values do not survive compression; BC6H is what would keep them.
