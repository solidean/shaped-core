---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
---

# The emitted text maps back to the SGL source

The emitted HLSL, MSL and WGSL are readable and keep SGL's names, but nothing maps a line back to the `.sgl` it came from.
So PIX, RenderDoc and Xcode step the emitted text, with every call inlined, and a target compiler's error names a line of text the author never wrote.

`#line` in HLSL and MSL would carry the mapping into each debugger and compiler message; WGSL has no such directive, so a source map beside the text is the alternative there.
Inlining is the hard part: a line of the emitted text can belong to a call site and to the body it inlined, and the mapping has to choose, or keep both.
