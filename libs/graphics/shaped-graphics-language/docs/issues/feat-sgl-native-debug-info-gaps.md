---
created: 2026-11-18
parent: epic-sgl-native-ir-targets
refs:
  - issue: feat-sgl-native-debug-variables
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/spirv/impl/writer_variables.cc
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/dxil/impl/writer_lines.cc
  - test: native-line-table-test
  - file: docs/plans/sgl-native-debug-variables.plan.md
claimed-by: Claude (for Julius)
claimed-at: 2026-11-18T10:51Z
---

# Native debug info: the variables full does not describe yet, globals, lexical blocks and columns

EMIT-267 landed with [feat-sgl-native-debug-variables](.closed/2026/11/feat-sgl-native-debug-variables.md): at `full` both native writers describe the locals the source wrote.
RenderDoc lists them on dx12 and vulkan, checked by `libs/graphics/shaped-graphics/tools/gpu-capture/check-debugger.py --debug-info full`.
This is what that issue asked for and did not reach, and what its plan found on the way.
The plan's IDs are kept, so its notes and log still say where each came from.

## Values a debugger shows as optimized out, or not at all

- **P3.9: a value no operation keeps whole**, such as a pixel stage's input struct `p`, reads as optimized out.
  Its pieces exist as stage loads; a description by pieces would show them.
- **P3.11: a promoted variable's value at a merge.**
  `construct_ssa` makes a phi per slot where a variable meets from two paths, and states no `debug_value` for it, so the debugger shows the value of one arm after the merge.
  sgl-cube's `light` after its loop is one.
- **P3.12: a debug value in an arm `flatten_branches` folds into a select** states the arm's value on both paths.
  It should be dropped there, or state the select.
- **P3.14: a let the legalizer splits into its lanes**, read by them alone, loses its name: `let v = float3(...)` read as `v.x`, `v.y`, `v.z` becomes lets `x`, `y`, `z` with no source name.
- **P4.7: DXIL writes no `llvm.dbg.declare`** for a variable a local in memory holds, an array indexed at run time among them.
  SPIR-V's `DebugDeclare` does.

## Scopes

- **P3.13: a call in a generic function's instance** names its function through the instance's tables, which the line table does not read yet.
- **P3.15: an inlined parameter's variable stands in the caller's scope**, where its binding stands, not in its callee's, as DXC's do.
- **Lexical blocks**: every variable is scoped to its function; `DILexicalBlock` and `DebugLexicalBlock` would scope a loop's or a branch's locals.

## Not described at all

- **Global resources**: `DIGlobalVariable` and `DebugGlobalVariable` for bindings, which DXC writes.
- **Columns**: every line states column 0 on DXIL and 1 on SPIR-V, since the line table carries no column.

## Not confirmed

- **PIX has not been checked to show a variable.**
  The automated check reads dx12's captures in RenderDoc; PIX only captured them, in P1.
  DXC's validator and disassembly read the `ILDB` part as PIX would, which is no proof that PIX shows the values.
