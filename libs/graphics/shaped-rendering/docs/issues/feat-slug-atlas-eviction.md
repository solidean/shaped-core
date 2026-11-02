---
created: 2026-10-17
parent: epic-sr-slug-open-items
refs:
  - symbol: sr::slug_atlas
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/slug_atlas.hh
---

# Slug atlas eviction

The atlas is append-only, so a font asked for many glyphs grows without bound.
Eviction has to rewrite every band list that points at a curve it moves, since lists hold texel locations.
