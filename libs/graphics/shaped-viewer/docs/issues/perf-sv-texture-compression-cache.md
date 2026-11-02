---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/docs/asset-loading.md
---

# Compression re-encodes on every load

A BC7 encode of a 4096x4096 chain is a few milliseconds of GPU time per texture, and Sponza's 72 take about 20 s of frames at the default work budget.
Caching the encoded blocks under the source's content hash, in `bcache`, is what asset-loading's "derived recipe" names for it.
