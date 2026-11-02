---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/layout/layout_tree.hh
---

# A title strip is cut, never shortened

A name wider than its leaf loses the glyphs that would run past the strip's edge, with no ellipsis.
Its look is fixed too — the default font at 13 logical pixels on a near-black band — until a `title_style` on the leaf is wanted.
