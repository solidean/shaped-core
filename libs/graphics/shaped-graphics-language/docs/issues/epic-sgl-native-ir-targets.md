---
created: 2026-11-06
refs:
  - file: libs/graphics/shaped-graphics-language/docs/architecture.md
  - file: libs/graphics/shaped-graphics-language/docs/issues/epic-sgl-shader-disassembly.md
  - file: libs/graphics/shaped-graphics-language/docs/issues/epic-sgl-optimizing-passes-and-ir-targets.md
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/ir.hh
---

# Native IR targets: SPIR-V and DXIL through one layer of our own

## Why

- **A path that does not depend on DXC**, for F3 and F8 above all.
- **A path a lot faster than DXC**, fast enough for a runtime compile and a hot reload.
- **Nothing in process that can crash us.** DXC is known to crash in process now and then.
- **Better DXIL in some scenarios later**, which the disassembly and simulation work can show and measure.
- **Features the text route does not expose**: motion traces, and reordering without inline SPIR-V.
- **`@float` on a block with no cost to the rest of its entry point**, which [feat-sgl-float-level-block-scoped](feat-sgl-float-level-block-scoped.md) waits for.

Compiling through DXC out of process would contain its crashes, and it runs counter to the speed this is for, so it is no plan.

## The shape

**One low-level layer, under both targets.**
SPIR-V and DXIL are both SSA over structured or structurable control flow, with typed resources and per-operation flags.
So the flat tree lowers once, after legalize, into [`ir::program`](../../src/shaped-graphics-language/ir/ir.hh), and SPIR-V and DXIL are two writers of it.
A SPIR-V writer over the flat tree would be rewritten for DXIL.
The optimizing passes of the parent epic live on that form, and disassembly and simulation read it.

**Flat tables, for a compile's speed.**
A program is arrays indexed by small ids: an operation is 24 bytes, its operands live in one array beside it, and no node is allocated on its own.
A type, a constant and a pointer are interned, each without a map of nodes.

**What a reader of the form can rely on.**

- Every float operation carries its own guarantee, so a guarantee narrower than a function needs no change to this form.
- Every operation keeps the flat statement it came from, and through it the `.sgl` line.
- Blocks are written in the order they open, which a structured tree makes a dominance order, and a block states its merge and continue targets.
- `plan` decides every name and address as it does for text, so the host binds one interface whichever route wrote the shader.

**The text targets stay, and stay the default.**
A package takes the route with `NATIVE_SPIRV`, where an entry point with no lowering is an error.
A vulkan context takes it with `vulkan_config::native_spirv`, where such an entry point goes through DXC.

**The flat tree is the right input.**
It is already structured, inlined and legalized, so SPIR-V's merge blocks come from what the tree says rather than from recovering them.
Mutable locals stay function variables for now, which SPIR-V allows and a driver promotes.
DXIL does not forgive that, so SSA construction arrives before DXIL does.

**A choice worth stating: memory has its own types.**
Vulkan refuses layout decorations on a value in a function, so a struct in GPU memory and the same struct as a value are two SPIR-V types, and a whole read or write crosses with `OpCopyLogical`.
This is why the module is SPIR-V 1.6, which needs Vulkan 1.3, the floor sg already holds.

## Order

1. **The layer and a compute-only SPIR-V writer**, behind a flag: done on 2026-11-06, see below.
2. **Measure on the G6 kernels and on compile time**: done, see below.
3. **Graphics stages, then ray tracing**, to parity with the DXC route on the corpus.
4. **Features**: motion traces, and SER written directly.
5. **DXIL**: LLVM 3.7 bitcode and the DXIL container, written by us.

The children say what stands between here and each.

## Measured on 2026-11-06

Every compute entry point of sg's and sr's shaders that lowers passes `spirv-val`: 95 of 128, the other 33 each a named gap, ray queries the most.
`tests/emit/spirv-emit-test.cc` holds that, and its count is a floor a lost lowering falls through.
The whole of sg's suite passes on vulkan through this route, as the `sg vulkan native-spirv backend` driver, under the validation layer.

A compile, release, Ryzen 9 9950X3D2, SGL's front end included on both sides, median of `slib - SGL's own SPIR-V against its text through DXC`:

| kernel | through DXC | the text alone | SGL's SPIR-V |
|---|---|---|---|
| a small dispatch | 2.96 ms | 0.43 ms | 0.42 ms |
| à-trous | 20.2 ms | 0.81 ms | 1.21 ms |
| SVGF | 171 ms | 4.82 ms | 4.87 ms |
| convolution | 43.1 ms | 2.70 ms | 2.74 ms |

So a compile is 2.5 to 35 times faster than DXC's route, and what is left is SGL's own front end rather than the writer.

Time per dispatch on an Arc B580, each over the same DXC build of the hand-written HLSL:

| kernel | SGL through DXC | SGL's SPIR-V |
|---|---|---|
| à-trous | 0.994× | 0.994× |
| SVGF | 0.982× | 0.983× |
| convolution | 0.971× | 0.973× |

**DXC's optimizer is worth nothing measurable on these kernels on this driver**, whose own compiler does the work.
So the optimizing passes need not lead on a desktop driver, and a kernel where this route loses is what would reopen that.

## What was answered, and what can stop or reshape this

- **D3D12 takes a DXIL no validator signed.**
  `dxil signing spike - which containers D3D12 takes` asked the system runtime on 2026-11-06, Windows 11 26300 on the Arc B580.
  A cleared digest and a `-Vd` compile are refused with `E_INVALIDARG`, and so is a changed byte under a signed digest.
  The retail bypass digest, sixteen bytes of `0x01`, is taken; the preview one, of `0x02`, is refused.
  So `dxil.dll` can go, and a container of ours writes that digest.
  The Agility SDK's runtime is unasked, since fetching it accepts Microsoft's license, which is the user's to do; this repeats by running the spike with it fetched.
  Signing would not have stopped the writer either way: only the dependency hangs on it.
- **The bug surface moves to us.** DXC's lowerings of odd cases, and what each driver's compiler happens to accept, become ours.
  A corpus compared across both routes is what holds it, which the second backend sweep is the start of.
- **Readable output** is lost on the native route, which is why the text route stays and `.sgl` lines are carried through.
