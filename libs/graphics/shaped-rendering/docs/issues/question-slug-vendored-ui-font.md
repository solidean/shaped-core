---
created: 2026-10-17
parent: epic-sr-slug-open-items
refs:
  - symbol: sr::slug_font::load_system_ui_font
  - symbol: sr::system_ui_font_path
  - file: libs/graphics/shaped-rendering/src/shaped-rendering/slug_font.cc
---

# Should shaped-core vendor a UI font?

`sr::slug_font::load_system_ui_font` reads a font the operating system ships, because shaped-core vendors none.
So what text looks like, and whether it loads at all, depends on the machine.
