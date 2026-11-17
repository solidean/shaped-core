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

- [x] P2.1 `debug_info` level `none` / `lines` / `full` in place of the `native_line_table` bool, through `text_request`, slib, `vulkan_config` and `dx12_config`
  - Outcome: `sgl::emit::debug_info` and `sg::native_debug_info`; slib's option `native_debug_info`; `sgl emit --debug-info`
- [x] P2.4 `SC_SG_NATIVE_DEBUG_INFO` overrides the config, so any program's shaders carry debug info for a capture (D6)
- [x] P2.2 `lines` writes today's bytes: the line-table tests unchanged under the new spelling
- [x] P2.3 EMIT-257 in the spec says the level; a new rule states `full`
  - Outcome: EMIT-267; `full` writes what `lines` writes until P3 and P4 land

## P3: The IR half, shared by both writers

- [ ] P3.1 `program::debug_variables`: source name, source type, kind, declaring statement; filled for `full` only
  - Note: a minted name cannot be told apart from a written one, and pass-made locals are not all `temporary`: `check`, `ends_search`, autodiff's `x`, `result`, `dx`
  - Note: so `flat_local` gains the name as written, set only where a declaration is flattened (P3.8)
- [x] P3.8 `flat_local::source_name`, empty for a local no source wrote; set at `let` and `var` statements, parameters, inlined parameter bindings, `for` counters
  - Outcome: `add_local`'s `written`; tests/check/source-name-check-test.cc pins the written names and a legalizer's local without one
  - Note: an inlined parameter whose argument is substituted, a literal or another local, is no local and so not described yet
  - Note: `flattener::add_local` in check/flatten.cc; parameters around flatten.cc:1006-1078, inlined bindings in flatten_call.cc:622 and 747
- [x] P3.2 The line table's call frames name their function, so a scope is a function rather than a file
  - Note: a frame is a `check::call_site`; its callee is `call_records[files[file].call_at(call)].callee`, through an instance's tables for a generic call
  - Outcome: `source_line::function`, filled by `source_lines::origin_of`; `ir::line_table::functions` and `source_place::function`
  - Note: a call inside a generic function's instance stands past the file's tables and names no function yet; its frame keeps the file's scope
- [ ] P3.3 `op::debug_value` and `op::debug_declare`, with `ir::verify`, `dump` and `operand_role`
- [ ] P3.4 Lowering writes them: a value at each source `let`, parameter and `for` counter, a declare at each `var`; temporaries none
  - Note: an immutable `let` that holds no array is no IR local but a value (lower.cc, `_locals[position] = expr(value)`), so its value op stands there
- [ ] P3.5 `construct_ssa` turns a promoted local's declare and stores into per-scalar `debug_value`s
  - Note: ssa.cc splits a promoted local into scalar slots (`add_slots`, `write_whole`); a store's value op names its first slot
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

### D6: An environment variable overrides the level (2026-11-17) [P2.4]
- **Context:** A capture by `SC_SG_CAPTURE` needs no change to the program, but its shaders carried no source unless the program's config asked.
- **Decision:** `SC_SG_NATIVE_DEBUG_INFO` overrides `dx12_config` and `vulkan_config::native_debug_info` when a context is created.
- **Why:** The two variables together debug any sg program at `.sgl` level with no code change; the P1 sample needed a local edit for it.
- **Alternatives:** A capture implying `full`, which couples two independent switches; config only.

## Deviations

## Log

- 2026-11-17: Plan created from the issue and a design conversation; D1 to D4 decided.
- 2026-11-17: Two red differential corpus entries fixed on the way to pushing P1, both ties a vendor breaks its own way.
- 2026-11-17: P2 done: one level through SGL, slib and sg, and `SC_SG_NATIVE_DEBUG_INFO` (D6).
- 2026-11-17: P3 started: its design notes in P3.1 to P3.5 and P3.8; P3.2 done, each line naming the function it stands in.
- 2026-11-17: P3.8 done: a local keeps the name its declaration wrote.
- 2026-11-17: P1 done but P1.6: `SC_SG_CAPTURE` (D5) and debug-pixel.py show RenderDoc reading EMIT-257's lines on dx12 and vulkan; PIX captures dx12.
