---
created: 2026-11-05
tags: [lsp]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/lsp.md
---

# Hover, go-to-definition, completion and formatting in sgl lsp

The language server answers diagnostics, semantic tokens, inlay hints and tests; it answers none of hover, go-to-definition, completion or formatting.
The checker's side tables already answer the first two per expression, and what the library lacks is a position-to-node lookup ([lsp.md](../lsp.md)).
Completion and a formatter are each a piece of their own after that.
The comparison matrix's F7 marks this gap; slangd and wgsl-analyzer both have hover and completion.
