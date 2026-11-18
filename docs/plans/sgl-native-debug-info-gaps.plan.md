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
- [ ] P1.2 A debug value in an arm `flatten_branches` folds into a select states the select, or is dropped (old P3.12)
- [ ] P1.3 A let the legalizer splits into its lanes keeps its source name, each lane described as a piece of it (old P3.14)
- [ ] P1.4 A value no operation keeps whole, a stage struct `p` above all, described by its pieces (old P3.9)
- [ ] P1.5 DXIL writes `llvm.dbg.declare` for a variable a local in memory holds, an array indexed at run time among them (old P4.7)

## P2: Scopes

- [ ] P2.1 An inlined parameter's variable stands in its callee's scope (old P3.15)
- [ ] P2.2 A call inside a generic function's instance names its function, through the instance's tables (old P3.13)
- [ ] P2.3 Lexical blocks: `DILexicalBlock` and `DebugLexicalBlock` scope a loop's and a branch's locals

## P3: What is not described at all

- [ ] P3.1 Global resources: `DIGlobalVariable` and `DebugGlobalVariable` for bindings, as DXC writes them
- [ ] P3.2 Columns: the line table carries a column, and both writers state it

## P4: Confirm and close

- [ ] P4.1 PIX shows an SGL local's value on a dx12 capture
- [ ] P4.2 check-debugger.py checks what P1 to P3 added: a value after a merge, a split let, a stage struct
- [ ] P4.3 Spec, architecture and native-parity docs say what landed; the issue closes

## Decisions

## Deviations

## Log

- 2026-11-18: Plan created from the issue; the issue claimed.
- 2026-11-18: P1.1 done: a variable takes its merge's value.
