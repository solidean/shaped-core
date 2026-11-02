---
created: 2026-10-17
parent: epic-sgl-modules-beyond-a-tracer
refs:
  - file: libs/graphics/shaped-graphics-language/tools/sgl/lsp_command.cc
  - file: libs/graphics/shaped-graphics-language/docs/lsp.md
---

# The language server reads its module directories once

It reads them from the `sgl.moduleDirs` setting at initialization, and re-reads their files on every analysis.
Go-to-definition into a module's file waits for the position lookup [lsp.md](../lsp.md) lists.
