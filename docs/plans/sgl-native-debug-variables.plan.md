---
title: SGL native debug variables
status: active
created: 2026-11-17
updated: 2026-11-17
issue: feat-sgl-native-debug-variables
---

# SGL native debug variables

## Goal

SGL's own DXIL and SPIR-V carry full debug info when asked: each source variable with its value and type, in the scope of the function it was written in, inlined calls included.
Done means a debugger shows an SGL local's value at a breakpoint, PIX on dx12 and RenderDoc on vulkan, checked by a script rather than by hand.
Asking for less writes what EMIT-257 writes today, byte for byte, and asking for nothing writes what it wrote before.
The issue is [feat-sgl-native-debug-variables](../../libs/graphics/shaped-graphics-language/docs/issues/feat-sgl-native-debug-variables.md).

## Context

- **The line table exists** (EMIT-257): `OpLine`s on SPIR-V, an `ILDB` with `DILocation`s on DXIL, `ir::line_table` between them.
- **An immutable `let` is no IR local**, only a value, so its description has to be attached where lowering evaluates it.
- **`construct_ssa` splits a promoted local into scalars**, so a variable's value after SSA is a set of scalar pieces.
- **The DXIL line table scopes by file**, one `DISubprogram` per file; variables need one per inlined function.
- **Guarantee kept from EMIT-257**: debug info changes no instruction, measured over every SGL compile in the repo.

## P1: Debugger automation, on the existing line table

Before anything goes on top of it, a script shows that a debugger reads what EMIT-257 writes.

- [x] P1.1 Find what drives PIX and RenderDoc headless on this machine: pixtool, renderdoccmd, RenderDoc's Python module
  - Outcome: RenderDoc 1.47's replay API, through `qrenderdoc --python`, debugs pixels on dx12 and vulkan; PIX 2610.28's API debugs shaders only from postmortem dumps (D5)
- [x] P1.2 A capture-ready SGL sample with the line table on, dx12 and vulkan
  - Note: sgl-cube under `--capture`, with `native_line_tables` switched on by a local edit, not committed; P2's level gives it a switch of its own
- [x] P1.5 `SC_SG_CAPTURE`: sg captures one epoch in RenderDoc or PIX, loaded in process before the first device (D5)
  - Files: libs/graphics/shaped-graphics/src/shaped-graphics/context/impl/gpu_capture.cc, docs/concepts/gpu-capture.md
  - Outcome: RenderDoc on dx12 and vulkan, PIX on dx12; reading a `.wpix` back through pixtool needs Windows developer mode
- [x] P1.3 A script that steps a pixel of a capture and reports each `.sgl` line and variable
  - Files: libs/graphics/shaped-graphics/tools/gpu-capture/debug-pixel.py
  - Outcome: sgl-cube's center pixel steps the same eight lines of cube.sgl on both backends, 41 to 59; DXIL with its inlined calls up to three frames deep, SPIR-V one frame
- [x] P1.4 File what the debuggers show wrong about the line table, if anything
  - Outcome: nothing wrong in the lines; every inlined DXIL frame reads `<inlined>`, with no function name, since the scopes are per file, which P3.2 fixes
- [ ] P1.6 A test that runs the capture and the replay where RenderDoc is installed, so the debugger check is repeatable
  - Note: no RenderDoc on CI; a manual nexus test or a dev.py command, to be decided

## P2: The request, as a level

- [ ] P2.1 `debug_info` level `none` / `lines` / `full` in place of the `native_line_table` bool, through `text_request`, slib, `vulkan_config` and `dx12_config`
- [ ] P2.2 `lines` writes today's bytes: the line-table tests unchanged under the new spelling
- [ ] P2.3 EMIT-257 in the spec says the level; a new rule states `full`

## P3: The IR half, shared by both writers

- [ ] P3.1 `program::debug_variables`: source name, source type, kind, declaring statement; filled for `full` only
- [ ] P3.2 The line table's call frames name their function, so a scope is a function rather than a file
- [ ] P3.3 `op::debug_value` and `op::debug_declare`, with `ir::verify`, `dump` and `operand_role`
- [ ] P3.4 Lowering writes them: a value at each source `let`, parameter and `for` counter, a declare at each `var`; temporaries none
- [ ] P3.5 `construct_ssa` turns a promoted local's declare and stores into per-scalar `debug_value`s
- [ ] P3.6 Every pass treats a debug op as no use and no content
  - Note: `remove_unused`, `share_common_values`, `hoist_common_values`, `flatten_branches`, `simplify`, `regroup_lanes`, `chain_sums`, `state_constant_tables`
- [ ] P3.7 Test: `full` against `lines` leaves every non-debug operation the same, over the repo's SGL

## P4: DXIL

- [ ] P4.1 Measure DXC's `-Zi` for variables on the DXC in `extern/dxc`: the metadata, `dbg.value`'s form in LLVM 3.7, vectors, matrices, structures, pieces
- [ ] P4.2 A `DISubprogram` per inlined function, with `inlinedAt` through them
- [ ] P4.3 Types: `DIBasicType`, `DICompositeType`, `DIDerivedType` as DXC writes SGL's types
- [ ] P4.4 `DILocalVariable` and `llvm.dbg.value` with `DW_OP_bit_piece` for a promoted scalar; `llvm.dbg.declare` for a local left in memory
- [ ] P4.5 DXC's validator and its disassembly of the `ILDB` take it; the `DXIL` part stays byte for byte
- [ ] P4.6 The P1 script shows a local's value in PIX

## P5: SPIR-V

- [ ] P5.1 `NonSemantic.Shader.DebugInfo.100`: import, `DebugCompilationUnit`, `DebugSource`, `DebugFunction`, `DebugScope`, `DebugInlinedAt`, `DebugLine`
- [ ] P5.2 Types: `DebugTypeBasic`, `DebugTypeVector`, `DebugTypeMatrix`, `DebugTypeComposite`, `DebugTypeArray`
- [ ] P5.3 `DebugLocalVariable` with `DebugValue` and `DebugDeclare`
- [ ] P5.4 `spirv-val` takes it, and every other id stays the one it is without it
- [ ] P5.5 The P1 script shows a local's value in RenderDoc

## P6: Close

- [ ] P6.1 Measure size and compile time of `full` against `lines` and `none` on the viewer's `path_trace`
- [ ] P6.2 Spec, architecture and native-parity docs say what landed; the issue closes

## Decisions

### D1: One level, `none` / `lines` / `full` (2026-11-17) [P2.1]
- **Context:** The line table is opt-in by a bool, mirrored through slib and both sg configs.
- **Decision:** One enum replaces the bool everywhere; `lines` is today's output.
- **Why:** Variables cost more output than lines, and lines alone stay worth having.
- **Alternatives:** The bool implying everything; a second bool that requires the first.

### D2: DXIL lands first (2026-11-17) [P4, P5]
- **Context:** Both writers share P3.
- **Decision:** P4 before P5.
- **Why:** DXC's `-Zi` is a reference to diff against, and PIX is the debugger on Windows.
- **Alternatives:** SPIR-V first; both in lockstep.

### D3: Only source-written locals are described (2026-11-17) [P3.4]
- **Context:** The flat tree's locals are `let`, `var`, parameters, `for` counters, and temporaries a pass introduced.
- **Decision:** The first four, under their names as written; a temporary is not described.
- **Why:** DXC hides its own temporaries, and a debugger's list stays what the author wrote.
- **Alternatives:** Every local, under its minted name.

### D4: The debugger check is scripted (2026-11-17) [P1]
- **Context:** The line table was verified by its format and DXC's tools alone; no debugger read it.
- **Decision:** A script drives PIX and RenderDoc, P1, before anything goes on top.
- **Why:** What a format check misses is what a debugger shows.
- **Alternatives:** Checking by hand; skipping it.

### D5: sg captures in process, by an environment variable (2026-11-17) [P1.5, P1.3]
- **Context:** Neither PIX nor RenderDoc can be scripted from outside without launching a built binary past dev.py, and PIX's API debugs no live capture.
- **Decision:** `SC_SG_CAPTURE` makes sg load RenderDoc or PIX before the first device and capture one epoch; RenderDoc's embedded Python replays it and debugs a pixel.
- **Why:** Any sg program becomes capturable headless and under dev.py, with no change to it, on both backends, and it outlives this plan.
- **Alternatives:** The capture code in one sample only; RenderDoc injecting from outside, which skips dev.py and needs a window.

## Deviations

## Log

- 2026-11-17: Plan created from the issue and a design conversation; D1 to D4 decided.
- 2026-11-17: P1 done but P1.6: `SC_SG_CAPTURE` (D5) and debug-pixel.py show RenderDoc reading EMIT-257's lines on dx12 and vulkan; PIX captures dx12.
