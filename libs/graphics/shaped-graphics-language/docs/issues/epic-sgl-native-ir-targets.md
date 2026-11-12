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
- **Nothing in process that can crash us.** DXC is known to crash in process now and then, and compiling out of process runs counter to the speed goal.
- **Better DXIL in some scenarios later**, which the disassembly and simulation work can show and measure.
- **Features the text route does not expose**: motion traces, and reordering without inline SPIR-V.
- **`@float` on a block with no cost to the rest of its entry point**, which [feat-sgl-float-level-block-scoped](feat-sgl-float-level-block-scoped.md) waits for.

Compiling through DXC out of process would contain its crashes, and it runs counter to the speed this is for, so it is no plan.

## The shape

**One low-level layer, under both targets.**
SPIR-V and DXIL are both SSA over structured or structurable control flow, with typed resources and per-operation flags.
So the flat tree lowers once, after legalize, into [`ir::program`](../../src/shaped-graphics-language/ir/ir.hh), and SPIR-V and DXIL are two writers of it.
The optimizing passes of the parent epic live on that form, and disassembly and simulation read it.

**Flat tables, for a compile's speed.**
A program is arrays indexed by small ids: an operation is 24 bytes, its operands live in one array beside it, and no node is allocated on its own.
A type, a constant and a pointer are interned, each without a map of nodes.

**What a reader of the form can rely on.**

- Every float operation carries its own guarantee, so a guarantee narrower than a function needs no change to this form.
- Every operation keeps the flat statement it came from, and through it the `.sgl` line.
- Blocks are written in the order they open, which a structured tree makes a dominance order, and a block states its merge and continue targets.
- `plan` decides every name and address as it does for text, so the host binds one interface whichever route wrote the shader.

**The native route is the default on dx12 and vulkan, and the text targets stay** (decided 2026-11-07).
A context prefers it unless created with `vulkan_config::native_spirv` or `dx12_config::native_dxil` off, which is the text route.
An entry point it does not lower goes through DXC, and slib logs which and why once, at info.
DXC stays for the foreseeable future: for a body or a type in a target's own text (CHK-432), and for FSR and NRD.
A package that must never reach DXC states `NATIVE_SPIRV` or `NATIVE_DXIL`, where an entry point with no lowering is an error.

**The flat tree is the right input.**
It is already structured, inlined and legalized, so SPIR-V's merge blocks come from what the tree says rather than from recovering them.
`ir::construct_ssa` promotes every local reached whole or at constant indices, scalar by scalar, for both targets.
A local an index reaches at run time stays a variable.

**DXIL's backend is tables, so what comes next is a row.**
`ir/dxil/` is general, and the four kernels it was measured on are no shape in it.

- `impl/llvm.hh` is an LLVM 3.7 module as flat tables, which knows nothing of DXIL: a new DXIL operation never touches it, and only a new LLVM instruction does.
  `llvm_bitcode.cc` writes it, and `llvm_passes.cc` sweeps what only exists once a value is its lanes.
- `impl/dx_ops.hh` is DXIL's operations: a row of `k_dx_ops` is an operation, and a row of `k_dx_classes` a signature, spelled as text.
  An overload a row does not list is a named gap, so a type no operation takes never reaches a driver.
- `writer.cc`'s `k_rules` says how each `ir::op` lowers, and `writer_math.cc`'s says the same of each `ir::math`.
  One function per case stands behind the rows that need more than one operation.
- `writer_resources.cc`'s `rule_of` is one row per kind of binding, and its `k_rules` tables cover atomics and subgroups.
- `writer_stage.cc` holds all that differs per stage, so a graphics stage adds there: a `stage_rule`, and its built-ins in `k_built_ins`.
  `signature.cc` is the one place that knows a row, and DXC holds it to account (EMIT-235).
  Its `k_features` is a row per feature a shader flag or the container states.
- `container.cc` is a list of parts, each a function; `ISG1` and `OSG1` come from `pack_signature`'s result, and are empty for compute.
  A validator rebuilds them and `PSV0` from the module and compares bytes, so their layout is DXC's to the byte.

Every value is a run of scalars in the writer, which is what DXIL asks and what lets vectors, matrices and structs share one path.
Memory is three kinds of place: arrays of scalars by leaf, for variables and workgroup memory; rows, for constant blocks; an element and a byte offset, for buffers.
What SPIR-V and DXIL share lives in `ir/`: `lower*.cc`, `ssa.cc` and `dominance.cc`.
The passes live there too, in `passes.hh`, with `simplify.cc` and `flatten.cc` for what only an unstructured target may do.

**A choice worth stating: passes work on values as they stand, and no scalarize step exists.**
`construct_ssa` already splits what crosses a block into scalars, so a sum or a phi the passes care about is one before a writer sees it.
A pass counts a vector's lanes where cost matters, in `flatten.cc`'s `cost_of`.
What is left is redundancy between lanes, which only the writer's expansion makes.
A row of constants two blocks read is one, and so are `length`'s dot and `normalize`'s, and one lane of a vector operation beside a scalar one.
The module's last sweep takes that, and it is worth 268 of SVGF's 2,559 instructions.
A scalarize step would have to move all of `writer_math.cc` and the memory lowering to replace one small pass.
A DXIL line table will have to carry each lane's statement through that sweep.

`ir/spirv/` is shaped the same way: `impl/spv.hh` is SPIR-V's numbers, and a new case is a row there too.
`writer_operations.cc`'s `k_rules` says how each `ir::op` is written and its `k_groups` each `subgroup`, and `writer_declarations.cc` holds types, globals and `k_built_ins`.
`ir/pipeline.hh` is each target's order of passes, one plain function per target, and `ir::verify` runs inside it wherever assertions are on.

**A choice worth stating: memory has its own types.**
Vulkan refuses layout decorations on a value in a function, so a struct in GPU memory and the same struct as a value are two SPIR-V types, and a whole read or write crosses with `OpCopyLogical`.
This is why the module is SPIR-V 1.6, which needs Vulkan 1.3, the floor sg already holds.

## Order

1. **The layer and a compute-only SPIR-V writer**, behind a flag: done on 2026-11-06, see below.
2. **Measure on the G6 kernels and on compile time**: done, see below.
3. **Graphics stages, then ray tracing**, to parity with the DXC route on the corpus, SPIR-V and DXIL in near-lockstep: [feat-sgl-native-graphics-stages](feat-sgl-native-graphics-stages.md).
   [feat-sgl-native-graphics-stages](feat-sgl-native-graphics-stages.md) is the stage interface's design, and what was measured of DXC's packing and of D3D12's link check.
   Vertex and pixel are done on both writers, 65 of the 66 entry points, and link against DXC's stages in both directions; a ray query is the one left.
   Compute stands at 93 of the 112 on DXIL and 98 on SPIR-V.
4. **Features**: motion traces, and SER written directly.
5. **DXIL**: LLVM 3.7 bitcode and the DXIL container, written by us: compute done on 2026-11-06, see below.

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

So a compile is 7 to 35 times faster than DXC's route, and what is left is SGL's own front end rather than the writer.
These predate SSA construction on this route; the sweep below ran with it.

Time per dispatch on an Arc B580, each over the same DXC build of the hand-written HLSL:

| kernel | SGL through DXC | SGL's SPIR-V |
|---|---|---|
| à-trous | 0.994× | 0.994× |
| SVGF | 0.982× | 0.983× |
| convolution | 0.971× | 0.973× |

**DXC's optimizer is worth nothing measurable on these kernels on this driver**, whose own compiler does the work.
So the optimizing passes need not lead on a desktop driver, and a kernel where this route loses is what would reopen that.

### DXIL

The kernels of the same benchmark, `slib - SGL's own DXIL against its text through DXC`, the same machine; the writer costs 0.05 to 0.64 ms over the text:

| kernel | through DXC | the text alone | SGL's DXIL | faster by |
|---|---|---|---|---|
| a small dispatch | 3.32 ms | 0.30 ms | 0.34 ms | 9.7× |
| à-trous | 10.4 ms | 0.78 ms | 1.05 ms | 9.9× |
| SVGF | 53 ms | 3.39 ms | 4.03 ms | 13× |
| convolution | 23.4 ms | 1.87 ms | 2.27 ms | 10× |

Time per dispatch on an Arc B580 with warm driver caches, six runs, each over DXC's build of the hand-written HLSL:

| kernel | SGL through DXC | SGL's DXIL | spread of SGL's DXIL |
|---|---|---|---|
| à-trous | 1.0023× | 0.9874× | 0.9870 to 0.9877 |
| SVGF | 0.9946× | 0.9841× | 0.9837 to 0.9845 |
| convolution | 1.0001× | 1.0093× | 1.0090 to 1.0099 |

The outputs agree with SGL's text through DXC: SVGF and the convolution to the bit, and à-trous in 132 of 4.2 million floats, by at most 1.2e-7.

| kernel | instructions, DXC | instructions, SGL | DXIL part, DXC | DXIL part, SGL | container, DXC | container, SGL |
|---|---|---|---|---|---|---|
| a small dispatch | 34 | 36 | 2,032 | 2,308 | 4,340 | 2,576 |
| à-trous | 308 | 301 | 4,484 | 5,160 | 7,456 | 5,552 |
| SVGF | 2,316 | 2,291 | 18,080 | 20,884 | 20,984 | 21,252 |
| convolution | 714 | 666 | 6,168 | 6,404 | 8,856 | 6,724 |

SGL writes no abbreviations yet, which is the larger DXIL part; a container holds no reflection or hash.
80 of the 112 compute entry points lower and pass the validator, and sg's whole suite passes on dx12 through this route under the debug layer.

**Read a new binary twice.**
The first run of a binary this driver has not seen reads up to 16% slow, 1.018×, 1.164× and 1.10× here, and every later one reads as above, across processes.
This is the driver's cold tier and not the code: a run with the module's passes off reads the same 0.989× and 0.985× once warm.
So a benchmark that rebuilt a shader compares a cold binary with a warm one, which is how this looked like a 16% loss before it was found.

**Only one pass answers for time on this driver.**
With `ir::simplify` alone and warm caches, à-trous and SVGF already read 0.989× and 0.985×, and the convolution 1.085×.
`chain_sums` takes the convolution to 1.010×, because a running total meeting each product alone fuses into `mad`.
What shares values, joins blocks and turns branches into selects moves no time here.
It brings the instruction counts level with DXC, which a less capable driver may need, and it costs part of the writer's time.

**The move to `ir/` cost nothing measurable, on 2026-11-06.**
A process lands in one of two modes about 1.5× apart, so a median across runs says nothing here.
This is the best of 400 compiles in one process, the writer and its passes over the text, in the fast mode:

| kernel | passes on the module | passes on `ir::program` |
|---|---|---|
| a small dispatch | 0.017 ms | 0.020 ms |
| à-trous | 0.196 ms | 0.212 ms |
| SVGF | 0.620 ms | 0.638 ms |
| convolution | 0.355 ms | 0.351 ms |

Two more `br` in à-trous are the whole change in instructions: a block of `extract`s alone is empty to the module, and not to a pass of `ir/`.
SPIR-V's route takes `share_common_values` and `remove_unused` for up to 3% more compile time and nothing measured in return, so it runs neither.

**The remaining 1% of the convolution is still unexplained, with less room to hide.**
Against DXC's DXIL from the same text, the loads, the products and the sums are the same in number, and both add one chain.
No speculation at all, `k_most_speculated = 0`, reads the same 1.0094×, so ours having 9 more selects and 16 fewer branches is not it.
What still differs: DXC stands every addition at the end of its block, after all the products, and writes `shl` and `or` where ours has `mul` and `add`.

## What was answered, and what can stop or reshape this

- **D3D12 takes a DXIL no validator signed.**
  `dxil signing spike - which containers D3D12 takes` asked the system runtime on 2026-11-06, Windows 11 26300 on the Arc B580.
  A cleared digest and a `-Vd` compile are refused with `E_INVALIDARG`, and so is a changed byte under a signed digest.
  The retail bypass digest, sixteen bytes of `0x01`, is taken; the preview one, of `0x02`, is refused.
  So `dxil.dll` can go, and a container of ours writes that digest, which sg's route has loaded under the debug layer.
  The Agility SDK's runtime, 1.620.1, takes the retail digest too: SGL's ray-tracing libraries, SM 6.9 hit objects among them, create state objects on it under the debug layer, on 2026-11-07.
  The spike itself creates its device without sg, so it still asks the system runtime, and the preview digest is unasked on Agility.
  Signing would not have stopped the writer either way: only the dependency hangs on it.
- **The bug surface moves to us.** DXC's lowerings of odd cases, and what each driver's compiler happens to accept, become ours.
  A corpus compared across both routes is what holds it, which the second backend sweep is the start of.
- **Readable output** is lost on the native route, which is why the text route stays and `.sgl` lines are carried through.
