---
created: 2026-11-01
tags: [babel]
refs:
  - file: libs/data/babel-serializer/src/babel-serializer/image/impl/stb_backend.cc
---

# babel::jpg re-encodes a JPEG with far more loss than libjpeg at the same quality

babel::jpg encodes through stb_image_write, whose quantization tables and chroma handling differ from libjpeg's.
Re-encoding an existing JPEG at its own quality therefore changes every block, where libjpeg nearly reproduces the file.

Measured in shelley (48th Ave's travertine, a 3700 x 2100 JPEG at quality 75), FLIP of the re-encode against the decoded source:

- **libjpeg (Pillow) at quality 75**: mean 0.0001 to 0.0005 in lit shots of the scene, a near copy.
- **babel::jpg at quality 75**: mean 0.056 over the views shelley's texture sizing weighs, enough to fail its 0.05 target.

stb also writes baseline Huffman tables, never optimized ones, so its files run larger than libjpeg's at the same quality.

Wanted: an encoder that matches libjpeg's quality scaling and optimizes its Huffman tables, so a re-encode at the source's quality is close to lossless.
Candidates: libjpeg-turbo (BSD-style, large, SIMD in assembly), or jpeg-compressor / jpgd style single files; mozjpeg compresses smaller still.
