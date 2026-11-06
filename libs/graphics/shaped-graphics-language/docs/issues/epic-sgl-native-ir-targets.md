---
created: 2026-11-06
refs:
  - file: libs/graphics/shaped-graphics-language/docs/architecture.md
  - file: libs/graphics/shaped-graphics-language/docs/issues/epic-sgl-shader-disassembly.md
---

# Native IR targets: SPIR-V and DXIL through one layer of our own

## Why

- **A path that does not depend on DXC**, for F3 and F8 above all.
- **A path a lot faster than DXC**, fast enough for a runtime compile and a hot reload.
- **Nothing in process that can crash us.** DXC is known to crash in process now and then, and compiling out of process runs counter to the speed goal.
- **Better DXIL in some scenarios later**, which the disassembly and simulation work can show and measure.
- **Features the text route does not expose**: motion traces, and reordering without inline SPIR-V.
- **Per-operation float guarantees**, which [feat-sgl-float-level-block-scoped](feat-sgl-float-level-block-scoped.md) is blocked on.

## The shape, proposed

**One low-level layer, under both targets.**
SPIR-V and DXIL are both SSA over structured or structurable control flow, with typed resources and per-operation flags.
So the flat tree lowers once, after legalize, into an SSA form of our own, and SPIR-V and DXIL are two writers of it.
A SPIR-V writer of its own would be rewritten for DXIL.
The optimizing passes of the parent epic live on that form, and disassembly and simulation read it, with each operation keeping its `.sgl` line.

**The text targets stay, and stay the default.**
A native target is opt-in per package until it has earned the default, as `SOFTWARE_TRACE` is, and the text route is what a native route is held to.

**The flat tree is the right input.**
It is already structured, inlined and legalized, so SPIR-V's merge blocks come from what the tree says rather than from recovering them.
The first slice may keep locals as function variables, which SPIR-V allows and a driver promotes.
DXIL does not forgive that, so SSA construction arrives before DXIL does.

## Order

1. **The layer and a compute-only SPIR-V writer**, behind a flag.
   Checked by `spirv-val`, and run on sg's vulkan compute tests beside the DXC route.
2. **Measure on the G6 kernels and on compile time.**
   This answers how much DXC's optimizer is worth once a driver has had its turn, which decides how soon the passes matter.
3. **Graphics stages, then ray tracing**, to parity with the DXC route on the corpus.
4. **Features**: motion traces, and SER written directly.
5. **DXIL**: LLVM 3.7 bitcode and the DXIL container, written by us.

## What can reshape this

- **Whether D3D12 takes a DXIL we wrote.** A container has carried a validator's hash.
  This decides whether `dxil.dll` can go, and not whether DXIL is worth writing: with a validator step it is still faster than DXC, and still ours to shape.
- **The bug surface moves to us.** DXC's lowerings of odd cases, and what each driver's compiler happens to accept, become ours.
  A corpus compared across both routes is what holds it.
- **Readable output** is lost on the native route, which is why the text route stays and `.sgl` lines are carried through.
