---
created: 2026-11-05
tags: [lsp]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/lsp.md
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/driver/references.hh
closed: 2026-11-05
resolution: fixed
---

# Hover, go-to-definition, completion and formatting in sgl lsp

Hover, go to definition and completion are done, on 2026-11-05: `sgl::reference_at` and `sgl::completions_at` answer them, and the server translates ([lsp.md](../../../../lsp.md)).
Left:

* **A formatter**, which is a piece of its own: the line tree decides most of the layout, so it is mostly spacing inside a line.
* **Completion from the last analysis that checked.**
  While the line under edit breaks its function's check, `x.` reads `x` as a name by its text, which misses `a.b.` chains.
* **Every overload in a hover, and signature help**, from the call record a call resolved through.

The comparison matrix's F7 marks the formatter as the gap; slangd and wgsl-analyzer have hover and completion, and neither has a formatter.
