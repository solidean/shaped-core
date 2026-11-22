# Migrating from HLSL

For a team, or an agent, porting an HLSL codebase to SGL.
It says what maps one to one, what SGL asks you to write differently and why, and the traps a port of a real renderer fell into.
The language itself is [the specification](spec/_index.md); this guide points into it rather than restating it.

## How to port

**Port shader by shader, each next to its HLSL twin, until a GPU test shows the two compute the same.**
Byte-identical output is the common case.
Where it is not, diff the DXIL of both: the usual cause is DXC reassociating differently (see [Numbers](#numbers)), which the diff explains every time.
Delete the HLSL once its twin test passes.

- `uv run dev.py run sgl -- emit <file> --entry <name> --target hlsl-dx12` shows the HLSL an entry point becomes; `--target spirv` and `--target dxil` write SGL's own binaries.
- `sgl describe` gives every binding, inline offset and sampler as JSON, which is what to generate host-side argument structs from instead of DXC reflection.
- **Time every port.** A correct port can still be slower, and only a profile shows it ([Performance](#performance)).

Every construct SGL does not carry yet is the diagnostic `unsupported-yet`, never a guess.
So a port either compiles to what you wrote or tells you where it stopped.

## What maps one to one

| HLSL | SGL |
|---|---|
| `[numthreads(8, 8, 1)] void cs(uint3 id : SV_DispatchThreadID)` | `@compute(8, 8) fun cs(@thread_id id: int3)` |
| `SV_GroupThreadID`, `SV_GroupIndex`, `SV_GroupID` | `@local_thread_id`, `@local_thread_index`, `@workgroup_id` |
| `cond ? a : b` | `if cond => a else b`, which evaluates only the side it takes |
| `select`-style both-sided choice | `select(cond, a, b)`, which evaluates both |
| `lerp`, `frac`, `rsqrt` | `mix`, `fract`, `inverse_sqrt` |
| `asuint`, `asfloat` | `reinterpret_as_uint`, `reinterpret_as_float` |
| `countbits`, `firstbithigh`, `reversebits` | `count_bits`, `first_bit_high`, `reverse_bits` |
| `mul(M, v)` | `M * v`, column vectors; HLSL written `mul(v, M)` multiplies a row vector, so check the convention before porting it |
| `out` and `inout` parameters | return a struct, or take `x: mut T` and call with `mut x` |
| `#define N 16` shared by arrays | a generic length, `fun f[n: int](v: float3[n])`, deduced at the call |
| `groupshared` | a `@workgroup binding` |
| `InterlockedAdd(c[i], 1)` | `c[i].add(1u)` on an `atomic[uint]`, which gives the value before |
| `GroupMemoryBarrierWithGroupSync()` | `workgroup_barrier()` |
| `WaveActiveSum`, `WaveActiveAnyTrue`, `WaveIsFirstLane` | `subgroup_add`, `subgroup_any`, `subgroup_elect`, behind `require subgroups` |
| `QuadAny`, `QuadAll` | `quad_any`, `quad_all`, behind `require subgroups, quad_control` (CHK-533) |
| `discard` / `clip(x)` | `discard`, as a statement or an arm: `if x < 0.0 => discard` |
| `t.Sample(s, uv)`, `SampleLevel`, `SampleGrad` | `t.sample(uv, s)`, `level = l`, `grad_x = dx, grad_y = dy` |
| `CalculateLevelOfDetail` | `t.lod(uv, s)` and `t.lod_unclamped(uv, s)` (CHK-535) |
| `SV_RenderTargetArrayIndex`, `SV_ViewportArrayIndex` | `@layer`, `@viewport_index` on a vertex stage's result, behind their features (CHK-419, CHK-534) |

## Bindings

An HLSL bindless renderer's `ResourceDescriptorHeap[i]` is an SGL binding array: declare exactly the arrays a shader reads, `maps: texture_2d[float4][64]`, and index them, `heap.maps[i]`.

- **A non-uniform index says so**: `heap.maps[nonuniform i]`; SGL proves the uniform ones and refuses an unmarked one it cannot prove (CHK-300).
- **An array whose length the host binds is `T[]`** (CHK-490), behind `unbounded_binding_arrays`; a huge fixed length is not needed.
- **Each array has one element type and one access mode**, so a buffer read in one shader and written in another is two members.
- **A resource may be named by a `let`**: `let t = heap.maps[nonuniform i]` (CHK-536).
  Inlining resolves every use to the element, with the index evaluated once, so no target sees a local of a resource type.
  A resource chosen at run time, `if c => a.x else a.y`, is refused; branch around its uses instead.
- Per-dispatch arguments as root constants are an `@inline binding`.

## Memory layout

**SGL places GPU memory at natural alignment on every target, which is not HLSL's structured-buffer packing** (EMIT-110).
The difference that bites is a 3-vector: `buffer[float3]` strides by 16 bytes where HLSL's `StructuredBuffer<float3>` strides by 12.

- **Such a buffer states its layout or does not compile** (CHK-532): `@padding(.none)` gives HLSL's 12, `@padding(.natural)` keeps 16.
  The same holds for `buffer[mat3]`, 36 against 48.
- A buffer of a struct needs nothing: the host writes the struct SGL generates, which carries every padding field.
- `@padding(.none)` on the binding covers every buffer in it.

## Derivatives and uniformity

**SGL refuses what HLSL silently accepts: a derivative where not every pixel of the quad arrives** (CHK-282).
That is an implicit `sample`, `ddx` or `t.lod` under a branch on a per-pixel value, whose result HLSL leaves undefined in a divergent quad.

- The fix that keeps the branch is to take the derivatives first and sample with them: `ddx(uv)` and `ddy(uv)` at the top, then `t.sample(uv, s, grad_x = dx, grad_y = dy)` inside the branch.
  A port that did this got the same image and a faster pass.
- **A branch on `quad_any(b)` or `quad_all(b)` reaches each quad whole, helpers included**, so a derivative may stand under it (CHK-533).
  `subgroup_any` does not give that: a helper invocation may sit out a subgroup operation.
- **A branch on what a subgroup agrees on, `subgroup_any(b)` or `subgroup_all(b)`, reaches each subgroup whole**, so a subgroup operation may stand under it (CHK-537).
  A derivative and a barrier still may not.

## Syntax that differs

- **Mixing two operators of one level needs parentheses** (OP-15): `(a & b) | c`, and `a and (b or c)`.
- **A splat of an operation is parenthesized**: `float4(..(a + b), 1.0)`; `..a + b` splats `a` alone and is refused.
- **Discard a result with `let _ = f(mut x)`**; a call of a function giving a value also stands as a statement.
- `half` is a type and `out` a keyword, so neither names a variable.
- A plain vector and a strong one convert with `as`, at no cost: `reflect(d as vec3, n as vec3) as float3`.
  The geometric builtins take `vec3`, `pos3` and `hpos4`, and the error of a call that passes `float3` says which `as` it needs.

## Numbers

**A port's floats can differ from the HLSL's by one ulp, and SGL is not wrong about either.**
An SGL argument loads where it is used, where hand-written HLSL often hoisted it, and DXC's fast math then reassociates the two programs differently.
The DXIL diff shows it each time; a kernel that compares against a threshold can amplify such an ulp into a different decision.
SGL's own DXIL and SPIR-V routes, `--target dxil` and `--target spirv`, round as SGL states rather than as DXC chose.

## Performance

**A local array indexed by a run-time value can spill to scratch memory**, which no compile error flags.
A mechanical port of a texture encoder whose per-subset arrays were indexed by a run-time subset ran 2.4 times slower until the arrays became separate locals chosen by branches.
The restructured port then ran faster than the HLSL, with byte-identical output.
Profile any port that indexes local arrays with something other than a loop counter SGL unrolls.

## Graphics stages

- **`@vertex_index` on dx12 adds the draw's start to `SV_VertexID`**, which reads `SV_StartVertexLocation`, a Shader Model 6.8 semantic, by default (EMIT-128).
  A host on Shader Model 6.6 compiles with the `root_constant` fill of `draw_start` and writes the start into a root constant per draw (EMIT-265, EMIT-271).
- Single-pass stereo draws through `@viewport_index` and sg's `set_viewports`, or through `@view_index` where the device has view instancing.
