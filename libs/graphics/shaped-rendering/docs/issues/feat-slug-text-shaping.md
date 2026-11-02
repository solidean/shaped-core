---
created: 2026-10-17
parent: epic-sr-slug-open-items
refs:
  - symbol: sr::layout_text
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/text_layout.hh
---

# Text shaping for `sr::layout_text`

`sr::layout_text` kerns, breaks and wraps lines, but does no shaping: no ligatures, no mark positioning, no reordering.
Scripts that need it — Arabic, Hebrew, Indic — come out as their glyphs in logical order.
It needs a design of its own before any code.
