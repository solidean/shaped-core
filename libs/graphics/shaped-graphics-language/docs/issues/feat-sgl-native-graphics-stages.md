---
created: 2026-11-06
parent: epic-sgl-native-ir-targets
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/emit/impl/plan.hh
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/ir.hh
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/emitting.md
---

# The native layer lowers the graphics stages

Vertex, pixel, then geometry, tessellation, mesh and task, with stage links and their locations from the plan, to parity with the text route on the corpus.
The sg sweep names what falls back to DXC.

## The problem

`ir::program` holds a compute stage's inputs as globals of `storage::input`, and has no output, no `discard` and no derivative.
SPIR-V and DXIL disagree on what a stage's edge is.
SPIR-V wants `Input` and `Output` variables, each with a `Location` or a `BuiltIn`.
DXIL wants `loadInput` and `storeOutput` calls naming a row and a column of a signature, stated three times: in the module's `dx.sig` metadata, in `ISG1` and `OSG1`, and in `PSV0`.

So a global with a `load` is the wrong shape: DXIL's writer would recover per-lane rows from a pointer.

## What the design rests on, measured

DXC 1.9.2602 on 2026-11-06, with the signature tables read from its disassembly, and D3D12's answers from `CreateGraphicsPipelineState` on the Arc B580.

**DXC packs a link, so one row per location does not link.**
`float3 a` and then `float b` share a row, and a later `float2` backfills an earlier row across a member of another mode.
Its default is `-pack-prefix-stable`, which the repo's arguments do not change.

**The rule is first fit, in declared order.**
EMIT-235 states it, and it predicts every element of every signature below, with nothing left over:

- An element takes the first row, and the lowest free columns, that hold it.
- A row holds one interpolation mode, which the pair of kind and sampling names.
- Types do not part a row: `float`, `int` and `uint` of one mode share one.
- An ordinary element never follows a system one in a row, and a system element may follow an ordinary one.
- `SV_Position` is a four-lane element like any other, at its place in the struct, and is `noperspective`.
- A vertex input takes one row per element, unpacked, and a stage input among them too.
- A render target takes the row of its index.
- `SV_TessFactor` stands in column `w` of rows from 0, and `SV_InsideTessFactor` follows: column `x` of row 3 for a triangle, column `w` of rows 4 and 5 for a quad.
- A link beside the factors takes the columns they leave, as a first fit from row 0.

`pack_signature` itself now holds to that, field by field, against DXC on 2026-11-06:

- **The repo's SGL:** 153 graphics entry points and 616 elements, with one entry point DXC refuses left out.
  Those are 90 pixel, 49 vertex, 5 mesh, 5 tessellation control, 2 tessellation evaluation and 2 geometry.
- **Twelve hand cases** of what no shader declares: links beside triangle, quad and line factors, `SV_PrimitiveID` in a tessellation stage, and each vertex and pixel system value.
- **250 random links**, 500 entry points and 6,132 elements, with `SV_Position`, `SV_RenderTargetArrayIndex`, `SV_IsFrontFace`, `SV_PrimitiveID` and `SV_SampleIndex` mixed in; a default run takes 30.
  In each, the vertex outputs agreed with the pixel inputs.

The first run agreed on every row, mask, system value and component type of the repo's shaders.
What it corrected was outside the rule.
A patch constant has no mode, and a line's first factor is its density.
`SV_PrimitiveID` is a signature element with no row in a geometry stage, and in no signature in a tessellation stage.

**D3D12 checks a link by name, then register, then mask, and refuses at pipeline creation.**
This holds with and without the debug layer, as `E_INVALIDARG`, so a wrong packing fails loudly rather than drawing garbage.

| vertex and pixel pair | answer |
|---|---|
| same text | created |
| a semantic renamed on one side | refused |
| members reordered on one side, which moves rows | refused |
| a stage input ahead of the link, which moves every row | refused |
| a stage input after the link | created |
| an interpolation mode that moves a row | refused |
| modes differing, registers equal: `noperspective` or flat against linear, linear against `centroid` or `sample` | created |

So only placement has to agree across routes, and interpolation does not.
A stage input after the link lands in a free column or a new row, and disturbs nothing, which is why CHK-271's order matters.

**`PSV0` is required, and its dependency table is checked.**
A container without `PSV0` is refused.
Without `STAT` and `HASH`, under the bypass digest, it validates and creates.
A zeroed input-to-output table still creates, but the validator calls it a mismatch with the module.
The debug layer runs that validator itself when it meets a pipeline's shaders, so a bad `PSV0` is an error in sg's tests rather than something they miss.

**Some of what looks like an element is an operation.**
`SV_ViewID`, `SV_StartVertexLocation`, `SV_StartInstanceLocation` and an input `SV_Coverage` are in no signature.
They are `dx.op.viewID`, `startVertexLocation`, `startInstanceLocation` and `coverage`.
`SV_SampleIndex`, `SV_Depth` and an output `SV_Coverage` are in a signature with no row.

**`discard` terminates on neither target.**
DXC writes `dx.op.discard(true)` and `OpDemoteToHelperInvocation`, and the code after it goes on running as a helper.
That is what keeps a neighbour's derivatives whole, and what [feat-sgl-discard-helper-emulation](feat-sgl-discard-helper-emulation.md) pins.

## The design

**A stage's edge is a table of elements, which lowering fills from the plan.**

```cpp
/// One thing that crosses the entry point's edge, a vector of up to four lanes at most.
struct sgl::ir::stage_element
{
    /// Which signature it stands in.
    stage_part part;          // input, output, patch, primitive
    built_in system;          // built_in::none for a link or a vertex attribute
    type_ref type;            // a scalar or a vector, never a matrix, an array or a struct
    interpolation interpolate;
    i32 location = -1;        // -1 for a system element
    u32 array_size = 0;       // per control point, per vertex or per primitive: 0 for one value
    u32 name_first, name_size;
    u32 semantic_index;       // `SGL`, 10
};
```

- **It is target-neutral.** It says what crosses and how; SPIR-V reads `location`, and DXIL reads the name and the index.
- **It has one entry per declared piece.** `plan::declared_members` already flattens EMIT-174's matrices and arrays and EMIT-196's 64-bit words.
  The text is written from that same list, so a piece is an element and no writer rebuilds it.
- **Order is meaning.** The table stands in declared order, a part's members then its stage inputs, because the packer reads that order.
- **Compute moves onto it.** `declare_stage_inputs` writes elements, `global::built_in` goes, and so does `storage::input`, so there is one path and no second one for graphics to grow beside.
  That landed, and with it one change of bytes.
  A variable's pointer type is the SPIR-V writer's now, declared ahead of its variable rather than in the program's type table.

**Two operations reach it.**

| operation | operands | SPIR-V | DXIL |
|---|---|---|---|
| `op::stage_load` | element, then an index where `array_size` is set | `OpLoad` through an access chain | one `loadInput` per lane, or the element's own operation |
| `op::stage_store` | element, value, then an index | `OpStore` | one `storeOutput` per lane |

A load is a pure value, so `construct_ssa` has nothing to promote and a dead one disappears.
The index carries a geometry stage's input vertex, a tessellation stage's control point, and a mesh stage's output vertex or primitive.

**Lowering is the same for every stage.**
The entry loads what the body reads and builds the parameter's value.
Each `return` stores each output element before `op::return_`.
The value goes through a local, as a compute stage's does, so a struct that is never read whole folds away.

**The semantic moves into the plan.**
`hlsl.cc`'s `semantic_of` is what both routes must agree on, so it becomes a plan function which the text writer and lowering both call, as `vertex_semantics` already is.

**Packing is DXIL's, in one function with a golden test.**
[`ir/dxil/signature.cc`](../../src/shaped-graphics-language/ir/dxil/signature.cc) is `pack_signature`, which turns a part's elements into rows and columns by the rule above; no other code knows a row.
SPIR-V never sees it, because a location is already the address (EMIT-26).
Everything else of a signature comes from that one result: `dx.sig`, `ISG1`, `OSG1`, and `PSV0`'s elements.
The usage masks are read off the final operations, after the passes, since a dead load is no use.

**The dependency table starts conservative.**
Both `dx.viewIdState` and `PSV0` state that every input reaches every output, and every output depends on the view where the stage reads it.
That is safe in the direction a table can be wrong, and costs no analysis.
Should the validator recompute it from the module, it becomes a bitset walk over the final operations.

**Which system element is which is one table in `writer_stage.cc`.**
Its `k_built_ins` gains how each `built_in` reaches DXIL, by stage: a signature element with a row, one with none, or an operation.

## The stage surface

| SGL | `ir` | SPIR-V | DXIL |
|---|---|---|---|
| `@position` | `built_in::position` | `Position`, `FragCoord` | `SV_Position` |
| `@interpolate` | `interpolation` | `Flat`, `NoPerspective`, `Centroid`, `Sample` | `dx.sig`'s mode |
| `@depth` | output element, plus a `depth_mode` on the program | `FragDepth`, `DepthGreater`, `DepthLess` | `SV_Depth` and its two conservative forms |
| `@sample_mask` output | output element | `SampleMask` | `SV_Coverage`, no row |
| `@sample_mask` input | input element | `SampleMask` | `dx.op.coverage` |
| `@sample_index` | input element | `SampleId` | `SV_SampleIndex`, no row |
| `@is_front_facing` | input element, `bool` | `FrontFacing` | `SV_IsFrontFace`, read as `uint` and compared |
| `@primitive_id` | input element | `PrimitiveId` | `SV_PrimitiveID` in a pixel stage |
| `@view_index` | input element | `ViewIndex` | `dx.op.viewID`, with `PSV0`'s flag and masks |
| `@layer` | output element | `Layer` | `SV_RenderTargetArrayIndex` |
| `@vertex_index`, `@instance_index` | input element | `VertexIndex`, `InstanceIndex` | `SV_VertexID` plus `startVertexLocation`, and the instance's alike |
| `@first_instance` | input element | `BaseInstance` | `startInstanceLocation` |
| `discard` | `op::discard`, no terminator | `OpDemoteToHelperInvocation` | `dx.op.discard(true)` |
| a derivative | `op::derivative`, `sub` naming axis and precision | `OpDPdx` and its family | `derivCoarseX` and its family |
| an implicit-level sample | new image operations | `OpImageSampleImplicitLod` and its family | `sample` and its family |

Three lines of that table hold behaviour rather than a name:

- **`VertexIndex` needs no base.** DXC's `-fvk-support-nonzero-base-vertex` subtracts the base and EMIT-128 adds it back, so the two cancel on SPIR-V, and only DXIL adds.
- **A pixel stage's `@position.w` is reciprocal on SPIR-V.** `-fvk-use-dx-position-w` writes `1 / FragCoord.w`, so the SPIR-V writer does it where it reads that element.
- **Conservative depth changes `@position`'s mode** in a pixel stage (EMIT-130). That row holds nothing else, and modes do not link, so the vertex stage needs no change.

`discard` goes on running, as the text does, so lowering does not treat it as the end of a path, and `has_effect` keeps it through every pass.
`flatten_branches` must not lift a store or a `discard` out of a branch, so both have to count in `ir::has_effect`.

**The later stages need one more operation each, and nothing new in the table.**

- **Geometry.** `op::emit_vertex` and `op::end_strip`, with inputs indexed by vertex and stores of the outputs ahead of each `emit_vertex`.
- **Tessellation.** The control stage's SGL body is what HLSL writes as the patch-constant function, and the hull function is a generated pass-through.
  So `program` stays one body, and each writer generates the pass-through from the element table.
  Patch constants are `stage_part::patch`, where DXIL's factors take their fixed rows first.
- **Mesh and task.** `op::set_mesh_counts`, `op::write_primitive` and `op::dispatch_mesh`, which is a terminator on SPIR-V, with outputs indexed by vertex or primitive.
  Per-primitive values are `stage_part::primitive`, packed on their own from row 0 in the mesh stage.
  The pixel stage packs them after its vertex rows, as one list.
  A task payload is workgroup-like memory, not a stage element.

Clip and cull distances are no part of SGL, so `pack_signature` does not hold their rule.

## What lands first

**SPIR-V and DXIL move in near-lockstep: a stage slice is done once both writers have it.**
**The mixed-route link test gates every slice from the first one**, since a stage that lowers and does not link is worth nothing.

1. **The plan function and the packer, with a golden test: done.**
   Every graphics entry point of the repo's SGL compiles through DXC, and its `ISG1`, `OSG1` and `PSG1` must equal `pack_signature`'s, field by field, with the mode out of the disassembly beside them.
   The test stands in slib's, [`sgl-signature-golden-test.cc`](../../../shaped-shader-library/tests/sgl-signature-golden-test.cc), since `ssc::dxc` does not link SGL.
   It holds before a stage lowers, and it stays as the guard on a DXC upgrade.
2. **`stage_element`, the two operations, and compute moved onto them: done.**
3. **Vertex and pixel, on both writers**: `stage_load`, `stage_store`, and each return storing its outputs.
   On DXIL that takes `dx.sig`, the signature parts and `PSV0`'s tables, with `spirv-val` and the validator on every entry.
   SPIR-V has no packing, so a fault only one writer shows is in that writer.
   **The mixed-route test lands with this slice**: each corpus pipeline's pair built both ways round, a DXC stage against a native one, through `CreateGraphicsPipelineState`.
   The golden test cannot say that the routes link; only this one can.
4. **The pixel surface, on both writers**: `discard`, derivatives, implicit-level sampling, then the rest of the table.
5. **Geometry and tessellation, then mesh and task, on both writers**, each with its corpus sweep and its pairs in the mixed-route test.

## Not confirmed

- **Whether the validator recomputes the dependency table from the module**, or compares `PSV0` with `dx.viewIdState` alone.
  It reported the mismatch against the module's table, and a run with the module's own metadata changed was not tried, since nothing here can edit bitcode.
  Step 4 answers it: a conservative table either passes or needs the bitset walk.
- **Whether a driver uses that table.** The runtime took a zeroed one, and a conservative one is untested on any driver.
- **Whether every path must store every output.** A path that ends in a `discard` stores nothing, and the validator's reading of a partial write is untried.
- **How D3D12 links a mesh stage to a pixel stage.** The rows differ by construction, and sg's mesh tests pass through DXC, but no mismatched pair was tried.
- **Geometry and tessellation pairs at pipeline creation.** Packing was measured there, and only vertex with pixel was linked by hand.
- **`-Od` and `-Zi`.** The sweep compiled at the default optimization level, with no debug info.
- **The Agility SDK's runtime and any GPU but the B580.** The link answers come from the D3D12 runtime rather than from a driver, so they should hold, but that is inference.
