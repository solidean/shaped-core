---
created: 2026-10-17
parent: epic-sr-slug-open-items
refs:
  - symbol: sr::slug_outline_of
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/slug_font.cc
  - file: libs/data/babel-serializer/src/babel-serializer/font/
---

# CFF and CFF2 outlines for Slug

`babel::font` reports CFF and CFF2 outlines and does not decode them, and `sr::slug_outline_of` refuses such a face.
That rules out most `.otf` files.
The charstrings decode to path commands in babel, and the cubics split into quadratics in sr (`slug_outline::cubic_to`).
So the work spans two libraries: babel-serializer's half comes first.
