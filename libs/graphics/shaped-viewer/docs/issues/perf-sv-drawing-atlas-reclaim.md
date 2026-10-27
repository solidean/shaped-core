---
created: 2026-10-17
parent: epic-sv-drawings-and-decals
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/drawing/drawing_manager.cc
  - symbol: sr::slug_atlas
---

# The drawing atlas reclaims space a page at a time

A page holding one long-lived set and much that is gone stays whole until it is the least recently drawn from.
Packing a page's survivors into a fresh one, or a free list in `sr::slug_atlas`, would reclaim the rest.
