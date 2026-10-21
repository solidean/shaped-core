---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-rendering/shaders/compression/bc.sgl
  - symbol: sr::bc_compress_routine
---

# BC7: the modes and partitions the encoder does not try

BC7 tries modes 6, 1, 3 and 7 over two candidate partitions.
Where its remaining quality is, each at a cost per block:
- a wider partition search;
- modes 0 and 2, three subsets;
- modes 4 and 5, rotated alpha.

Measured today: around 40 dB on the tests' noisy gradient.
