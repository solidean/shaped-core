---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/resources/impl/lru_pool.hh
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/impl/keyed_cache.hh
---

# Fold `lru_pool` onto `impl::keyed_cache`

`lru_pool` is `keyed_cache` plus minted ids plus a content-hash index.
Folding it in leaves sv carrying one eviction implementation rather than two.
