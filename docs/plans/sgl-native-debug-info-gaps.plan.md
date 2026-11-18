---
title: SGL native debug info gaps
status: active
created: 2026-11-18
updated: 2026-11-18
issue: feat-sgl-native-debug-info-gaps
---

# SGL native debug info gaps

## Goal

At `full`, a debugger shows every variable the source wrote with its right value wherever it is in scope, on dx12 and vulkan alike.
That covers values at merges and folded branches, split lets, stage structs, and locals left in memory, each in the scope it was written in.
Global resources and columns are described as DXC describes them, and PIX is confirmed to show a value.
`lines` and `none` stay byte for byte, and `full` still changes no instruction (EMIT-267).
The issue is [feat-sgl-native-debug-info-gaps](../../libs/graphics/shaped-graphics-language/docs/issues/feat-sgl-native-debug-info-gaps.md).
Its items keep the IDs of [the first plan](sgl-native-debug-variables.plan.md), named in each task.

## Context

- **What exists** is the first plan's: `op::debug_value` and `op::debug_declare` in the IR (its D7), DXIL's `llvm.dbg.value`, SPIR-V's `DebugValue` and `DebugDeclare`.
- **The guard** is native-line-table-test.cc: `full` against `lines`, every non-debug instruction the same over the corpus.
- **The debugger check** is `libs/graphics/shaped-graphics/tools/gpu-capture/check-debugger.py --debug-info full`, on sgl-cube.

## P1: Values a debugger shows wrong or not at all

- [x] P1.1 A promoted variable's value at a merge: `construct_ssa` states a `debug_value` of each phi it makes for a described slot (old P3.11)
  - Outcome: `lay_out` states each kept phi of a declared local's slot after the block's last phi; sgl-cube's `light` after its loop reads the merge's phi on vulkan
  - Note: only a phi a read made is described: a phi for the debugger alone would be an instruction `full` adds, so a variable written in both arms and never read after reads as the last arm's
  - Files: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/ssa.cc, tests/ir/passes-ir-test.cc
- [x] P1.2 A debug value in an arm `flatten_branches` folds into a select states the select, or is dropped (old P3.12)
  - Outcome: `drop_path_values` drops an arm's values of a mutable variable before it splices the arm; the merge phi's value (P1.1) becomes the select's
  - Note: a `let` of an arm keeps its value, which no path changes; `debug_variable::is_mutable` tells them apart
  - Commit: f739334a8
- [x] P1.3 A let the legalizer splits into its lanes keeps its source name, each lane described as a piece of it (old P3.14)
  - Outcome: `flat_local::piece_of` and `piece_member`, set by L4's `split_at`; lowering describes a piece as its whole from the piece's first leaf on
  - Outcome: the split lets stand on the construction's line, where they stood on line 1 before, at `lines` too
  - Note: the whole's type may be one no operation holds, which a writer must not declare (D1)
- [x] P1.4 A value no operation keeps whole, a stage struct `p` above all, described by its pieces (old P3.9)
  - Outcome: `remove_unused` states a dead `construct`'s live parts, each from its leaf on, where it wrote `none`; RenderDoc lists `p.color` and `p.normal` on dx12 and vulkan
  - Outcome: an entry point's parameter takes the body's first statement for its place, where it had none: dx12 put it in an unnamed scope with no location
  - Note: check-debugger.py expects `p` now; `program::leaves_of` replaces lowering's own count
- [x] P1.5 DXIL writes `llvm.dbg.declare` for a variable a local in memory holds, an array indexed at run time among them (old P4.7)
  - Outcome: the module builder's `debug_declare`, kept no more alive than `debug_value`; DXC's validator takes it and its disassembly reads `dbg.declare(metadata [3 x float]* ...)`
  - Note: a local of several kinds of scalar is several arrays, whose leaves no single declaration lays out; it is not declared yet

## P2: Scopes

- [x] P2.1 An inlined parameter's variable stands in its callee's scope (old P3.15)
  - Outcome: `flat_local::scope_through`, the calls the callee was inlined through; lowering places the parameter's variable and value at the first statement standing in exactly those calls
  - Outcome: sgl-cube's `color` and `normal` are `shade`'s on both backends, listed in its frame
  - Note: a statement id would not do: the legalizer compacts the tree into new ids and rewrites a body's statements, where it keeps the call sites and each statement's calls
  - Note: a callee whose body folds into the call's own statement has no statement of its own, so its parameters stay in the caller
- [x] P2.2 A call inside a generic function's instance names its function, through the instance's tables (old P3.13)
  - Outcome: `call_site::callee`, set where the inliner makes the site; `source_lines::origin_of` names a frame by it, where it read the file's tables
  - Deviation: no instance's tables are read; the inliner records the callee (X1)
- [x] P2.3 Lexical blocks: `DILexicalBlock` and `DebugLexicalBlock` scope a loop's and a branch's locals
  - Outcome: `line_table::blocks` and `source_place::block`, from a walk of the legalized tree; an `if` arm, a loop's body and a case's arm open one
  - Outcome: a statement stands in the innermost block of its own calls, so an inlined body starts again at its function; sgl-cube's `n`, `light` and `i` are listed only inside `shade`'s else arm
  - Note: a `for` counter is scoped where the `for` stands, not to its body as DXC scopes one

## P3: What is not described at all

- [ ] P3.1 Global resources: `DIGlobalVariable` and `DebugGlobalVariable` for bindings, as DXC writes them
- [ ] P3.2 Columns: the line table carries a column, and both writers state it

## P4: Confirm and close

- [ ] P4.1 PIX shows an SGL local's value on a dx12 capture
- [ ] P4.2 check-debugger.py checks what P1 to P3 added: a value after a merge, a split let, a stage struct
- [ ] P4.3 Spec, architecture and native-parity docs say what landed; the issue closes

## Decisions

### D1: A type only a debug variable names is flagged, and SPIR-V gives it no id (2026-11-18) [P1.3]

- **Context:** A split let's whole, `float2` of doubles say, may have a type no operation holds; lowering it for the debug variable added an `OpTypeVector` at `full`, which the EMIT-267 test caught.
- **Decision:** `ir::type::is_debug_only`, set on what lowering makes while describing, and cleared with what the type is made of once real code asks for it.
  The SPIR-V writer numbers and declares only the others.
- **Why:** Passes after lowering add types too (`state_constant_tables`), so the debugger's types cannot simply come last; DXIL interns its own types by use and needs nothing.
- **Alternatives:** A boundary index past which types are the debugger's; a separate table of debug types with references of their own.

## Deviations

### X1: A call site records its callee, in place of reading an instance's tables (2026-11-18) [P2.2]

- **Planned:** Name a call inside a generic function's instance through the instance's tables.
- **Actual:** `call_site::callee`, set where the inliner makes the site, and the line table reads it first.
- **Why:** The inliner knows the callee as it inlines, for every call; the tables would need the instance threaded through to `source_lines`.
- **Impact:** None beyond the field; a site made elsewhere still falls back on the file's tables.

## Log

- 2026-11-18: Plan created from the issue; the issue claimed.
- 2026-11-18: P1.1 done: a variable takes its merge's value.
- 2026-11-18: P1.2 done: a folded arm states no value of a path's variable.
- 2026-11-18: P1.3 done: a split let is one variable, its lanes pieces of it; D1.
- 2026-11-18: P1.1 to P1.3 shipped; P1.4 done: a stage struct shows the fields that are read.
- 2026-11-18: P1.5 done: DXIL declares a local in memory; P1 done.
- 2026-11-18: P2.1 done: an inlined parameter is its callee's variable.
- 2026-11-18: P1.4 to P2.1 shipped; P2.2 done: a call in a generic instance names its function.
- 2026-11-18: P2.3 done: lexical blocks; P2 done.
