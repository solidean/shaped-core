---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
closed: 2026-11-03
resolution: fixed
---

# The emitted text maps back to the SGL source

The emitted HLSL, MSL and WGSL are readable and keep SGL's names, but nothing maps a line back to the `.sgl` it came from.
So PIX, RenderDoc and Xcode step the emitted text, with every call inlined, and a target compiler's error names a line of text the author never wrote.

`#line` in HLSL and MSL would carry the mapping into each debugger and compiler message; WGSL has no such directive, so a source map beside the text is the alternative there.
Inlining is the hard part: a line of the emitted text can belong to a call site and to the body it inlined, and the mapping has to choose, or keep both.

## Design (agreed 2026-11-02)

- The text writer records a **line table** on every compile: each emitted line → its `.sgl` file and line, plus the inlined call chain from `flat_expr::inlined_through`.
  It serves all four targets, and [feat-sgl-gpu-print-and-assert](../../../feat-sgl-gpu-print-and-assert.md) reuses it for its site table.
- HLSL and MSL also carry `#line N "file.sgl"` wherever the origin changes, naming the innermost line; an inline boundary gets a `// inlined from a.sgl:12 › b.sgl:40` comment.
- What slib hands a driver carries the directives; `sgl emit` omits them unless asked, so the readable text stays readable.
- WGSL has no `#line`: slib's WGSL path translates Tint's and Dawn's messages through the line table, which is also the fallback for any message that lost a directive.

## Landed, and what is left (2026-11-02)

The line table (EMIT-181) and HLSL's and MSL's `#line` (EMIT-182) landed, and slib asks for the directives.
WGSL is left: slib never compiles it, since Dawn does at `wgpuDeviceCreateShaderModule` and reports through the device's error callback.
So the table goes into sg as a source map on `sg::compiled_shader`, beside the site table of [feat-sgl-gpu-print-and-assert](../../../feat-sgl-gpu-print-and-assert.md).
The webgpu backend then asks Dawn for the module's compilation info and logs each message at its `.sgl` line.

## Closed (2026-11-03)

`sg::compiled_shader::source_map` carries the line table for a text format, and slib's SGL edge fills it for WGSL.
The webgpu backend asks Dawn for each module's compilation info, never waiting, and logs every error and warning at its `.sgl` line and inlined calls.
The request is held in the context's backlog, and the messages go to the test or task that created the shader.
