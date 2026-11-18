---
created: 2026-11-17
parent: epic-sgl-native-ir-targets
refs:
  - issue: feat-sgl-native-line-table
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/spirv/impl/writer_lines.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/dxil/impl/writer_lines.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
  - test: native-line-table-test
closed: 2026-11-18
resolution: fixed
---

# Full debug info from the native writers: variables, types and scopes, past the line table

EMIT-257 gave both native writers a line table, opt-in through `text_request::native_line_table`.
It maps each operation to its `.sgl` line and stops there: a debugger can step through the source but cannot show a value.
This issue is the rest of what DXC's `-Zi` and `-fspv-debug=vulkan-with-source` give, so PIX and RenderDoc debug an SGL shader the way they debug HLSL.

## What exists

- **SPIR-V:** core `OpString`, `OpSource` with the text, `OpLine` per changed line.
  Core lines have no inlining, so an inlined statement names only its innermost line.
- **DXIL:** an `ILDB` part and an `ILDN` part, the module again with a `DILocation` per instruction, a `DISubprogram` per file with the `inlinedAt` chain, and `dx.source.contents`.
- **Neither** describes a variable, a type or a lexical scope, and columns are always 0.

## What is wanted

- **SPIR-V: `NonSemantic.Shader.DebugInfo.100`**, what DXC's `vulkan-with-source` writes and RenderDoc's shader debugger reads.
  `DebugCompilationUnit`, `DebugSource`, `DebugFunction` and `DebugScope`, `DebugInlinedAt` for the call chain core lines lose, and `DebugLine` with columns.
  `DebugLocalVariable` with `DebugDeclare` or `DebugValue`, and the types they name: `DebugTypeBasic`, `DebugTypeVector`, `DebugTypeMatrix`, `DebugTypeComposite` with members, `DebugTypeArray`.
  Global resources as `DebugGlobalVariable`.
- **DXIL: variables in the `ILDB` module.**
  `DILocalVariable` with `llvm.dbg.declare` or `llvm.dbg.value`, and `DIBasicType`, `DICompositeType` and `DIDerivedType` for SGL's types.
  `DILexicalBlock` for scopes, and `DIGlobalVariable` for resources.
  Whatever else PIX needs to show a value, `dx.dbg.varlayout` among the names the bitcode writer already knows, to be measured against DXC's `-Zi` output.
- **Columns**, where the line table can carry a statement's column.
- **Opt-in as before**, and the guarantees EMIT-257 measured kept: off writes the same bytes, on changes no instruction outside the debug ones.

## Known and open

- **The IR has the names.** `ir::local` keeps a variable's name and `ir::member` a member's, so the source of a description exists before SSA.
- **Hypothesis: SSA is the hard part.** Once a local becomes SSA values, a description has to follow it through `DebugValue` or `llvm.dbg.value` at each def.
  Whether the passes keep enough to do that, or each pass has to carry the variable along, is not yet looked at.
- **No debugger has read the line table yet** ([feat-sgl-native-line-table](feat-sgl-native-line-table.md#not-confirmed)).
  Driving RenderDoc and PIX on the line table first would show what the format checks miss, before more goes on top of it.
- **Done means a debugger shows it**: a local's value at a breakpoint in RenderDoc on vulkan and in PIX on dx12, with `spirv-val` and DXC's validator still passing.
