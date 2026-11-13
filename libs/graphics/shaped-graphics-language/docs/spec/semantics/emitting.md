# Emitting

*Tracer: deliberately thin.*

An emitter writes the text a graphics API compiles, from one flat tree of a [checked module](checking.md#the-flat-tree).
It carries what [cube.sgl](../../../tests/samples/cube.sgl) needs, and compute entry points, groups, buffers, enums and `case` besides; every other construct is `unsupported`.
The cube's text has met its readers: DXC compiles both HLSL targets, and WebGPU compiles the WGSL.
[sgl-cube](../../../../../../examples/graphics/sgl-cube/sgl_cube.cc) draws the same picture on all three, and no rule below had to change for it.
**The MSL text has met its reader too**: slib's Metal compiler compiles it, and sg's tier-1 tests draw and dispatch with it on a Metal device.
Back to the [semantics](_index.md); the reasons are in [why/emitting.md](why/emitting.md).

## Targets

* **EMIT-1** A **target** is a text format together with the addressing rules of the backend that reads it.
* **EMIT-2** The targets are `hlsl-dx12`, `hlsl-vulkan`, `wgsl` and `msl` ([why](why/emitting.md#emit-2)).
* **EMIT-3** Every target's text carries its final addresses: no later pass numbers a binding, a location or an offset.
* **EMIT-4** No target depends on a flag of the compiler that reads its text for its meaning ([why](why/emitting.md#emit-4)).
  A text means what it says under every flag, or it fails to compile without the one it needs, and never means something else.
  HLSL that names a 16-bit type needs DXC's `-enable-16bit-types`, which slib's SGL edge passes on every compile.
  MSL that holds a coherent member or an image atomic needs the language version that has it (EMIT-150, EMIT-151).
  slib's SGL edge has a metallib compiled against MSL 3.2, and the driver compiles source at the newest version the device has.
* **EMIT-5** One emission writes one entry point: that entry point, and exactly the structs and the binding it needs.
* **EMIT-6** Nothing in the text of one entry point depends on the text of another ([why](why/emitting.md#emit-6)).
* **EMIT-7** An emitter reads the flat tree, the module's types and the module's bindings, and never an AST.
* **EMIT-65** An emitter reads a tree in the [core form](legalization.md#the-core-form), and it transforms nothing: the legalizer runs in front of it.
* **EMIT-72** Emitting an entry point of a checked module legalizes it first, since the check pass writes the structured form; a tree handed over by itself is core, or it is EMIT-66.
* **EMIT-8** Emitting is deterministic: one checked module, one entry point and one target give one text.

## Errors

* **EMIT-9** Emitting is total: its result is the text, or a list of errors and no text.
* **EMIT-10** A module that reported an error is the error `module-has-errors`, whichever entry point is asked for.
* **EMIT-11** An entry point the module does not hold is `unknown-entry-point`.
* **EMIT-12** A construct that no emitter carries yet is `unsupported`, and its detail names the construct; an emitter never guesses an address.
* **EMIT-13** No error depends on the target but EMIT-109's, EMIT-122's and EMIT-261's: an entry point is written for every target or for none ([why](why/emitting.md#emit-13)).
* **EMIT-109** An entry point that needs a feature no device of the target has is `target-lacks-feature`, and its detail names the feature.
  Today that is `wgsl` against `binding_arrays`, `multisampled_array_textures`, `raytracing_pipeline`, `geometry_shader` and `tessellation_shader`.
  It is also `wgsl` against `shader_int16`, `device_coherence`, `image_atomics`, `first_instance` and `helper_lane_query`, which core WebGPU has none of.
  And it is `wgsl` against `shader_int64`, `shader_f64` and `shader_int64_atomics`, which no WebGPU has.
  `ray_query` is none of them: WGSL writes a trace's emulated form (EMIT-135).
  The shader chose it by `require`, so a portable shader still meets EMIT-13's promise.
* **EMIT-66** A tree that is not core is the error `not-core`, and its detail names the first node that offends.
* **EMIT-67** A `print` is `unsupported` without diagnostics, which is why the driver legalizes every entry point without them there (LEGAL-63).
  A description holds whichever a compile asks for, so no print keeps `sgl describe` from writing one.

## Names

* **EMIT-14** Every name an emitter writes comes from the entry point's mint ([CHK-104](checking.md#the-flat-tree)).
* **EMIT-15** Each target has a list of **reserved words**: its keywords and its predeclared types, together with every name a builtin writes in that target (EMIT-74).
  That is the function a call is written as, and every name a writer of its own calls, declares or reaches into, which its record lists per target.
* **EMIT-16** The reserved words of the target are taken in the mint before anything else is minted.
* **EMIT-17** A struct, a binding or a local whose name is reserved in a target is minted from that name and a trailing underscore, in that target only.
  Every target reserves each name that starts with `sgl_`, which is where an emitter's own names stand, so `sgl_data` is written `sgl_data_`.
  An entry point the check pass adds under such a name keeps it (CHK-345).
* **EMIT-96** Two structs of one name, which the program's file shadowing one of the prelude's gives ([CHK-188](checking.md#symbols)), are written under two names: the one written later is minted.
  The same holds for the cases of two enums of one name.
* **EMIT-18** A member whose name is reserved gets trailing underscores until it is free among its siblings, in that target only.
* **EMIT-19** A `@builtin` declaration is never written by its name: each target spells it as its record in the builtin registry says (EMIT-74).
* **EMIT-20** An entry point keeps its name where the target allows it, and where the target reserves it, it is minted like any other name (EMIT-17); the text reports the name it declares.
* **EMIT-21** *Retired:* `reserved-entry-point-name` is no longer reported, since EMIT-20 renames the entry point instead ([why](why/emitting.md#emit-21)).

The struct `target` of the cube is reserved in WGSL and nowhere else.

```wgsl
struct target_ {
    @location(0) color: vec4f,
}
```

## Types

* **EMIT-22** A struct of the program keeps its name, and a builtin type is spelled as its record says, which today is the table below.
* **EMIT-179** An instance of a generic struct is named after its template and its arguments, `pair_float_int` for `pair[float, int]`, with every character no identifier holds written `_` (CHK-339).
  Instances of one template whose members came out alike are one struct, named after the first a tree needs: after the front end nothing tells them apart.
* **EMIT-23** A struct is declared after every struct it holds.
  It is declared where something the body reaches holds one, or where a binding or the stage edge does: a struct LEGAL-61 split into its fields is declared nowhere.
* **EMIT-106** A field of type `void` holds nothing, so no target declares it, and a construction writes no argument for it.
* **EMIT-107** A struct whose every field is `void` is `unsupported`, since it would be a struct of no member, which WGSL has no spelling for.
* **EMIT-118** An array is `array<T, N>` in WGSL and MSL, and in HLSL its element type with the lengths after the declared name, `float weights[3]`.
  A square literal is WGSL's constructor, and in HLSL and MSL a local assigned element by element, as a struct is (EMIT-55).
* **EMIT-230** In HLSL, a `let` array whose every element is a literal, or a vector or an array of them, is `static const` with an initializer: `static const float kernel[5] = {0.0625, ...};`.
  Each invocation filling a local of its own costs a store per element where an index into it is dynamic, which DXC's SPIR-V keeps.
* **EMIT-231** `T[N].filled(v)` (CHK-289) of at most 64 elements in all is an assignment per element at a constant index, and a larger one a loop per level.
  A loop's dynamic index keeps the array in memory wherever the target's compiler does not unroll it, as DXC's SPIR-V backend does not.
* **EMIT-119** Each member of a `@workgroup` binding is a variable of its own, minted `<binding>_<member>`: `groupshared` in HLSL and `var<workgroup>` in WGSL at file scope,
  and `threadgroup` at the top of the kernel in MSL, which has it nowhere else.
  It takes no group, so the groups after it in a list keep their numbers.
* **EMIT-120** An atomic is `atomic<u32>` in WGSL and `atomic_uint` in MSL, updated by `atomicAdd(&a, v)` and `atomic_fetch_add_explicit(&a, v, memory_order_relaxed)`.
  HLSL declares the plain integer, and its `Interlocked*` gives the value before through an out parameter, so a local declared ahead of the statement holds it.
  A load is `InterlockedOr` with 0, and a store `InterlockedExchange`, so that no plain access races an update.
* **EMIT-161** A compare-exchange is HLSL's `InterlockedCompareExchange(a, expected, desired, before)`, which is strong, for either form.
  The weak form fills `exchange_result[T]`, a struct every target declares as the program's own, from it with `exchanged` as `before == expected`.
  WGSL's `atomicCompareExchangeWeak` and MSL's `atomic_compare_exchange_weak_explicit` are weak, and the weak form is one call of it ([why](why/emitting.md#emit-161)).
  WGSL fills the struct from its result's `old_value` and `exchanged`, and MSL from its `bool` and the value it wrote back into `expected`.
  The strong form there is a loop over that call, which retries a failure that read `expected` and stops at any other.
  The atomic's place, `expected` and `desired` are each evaluated once, in that order, ahead of any loop.
  An `@atomic` image's texel is the same in MSL, over its texture's `atomic_compare_exchange_weak`, with the coordinate in place of the pointer.
* **EMIT-162** In a pixel stage, the strong form's loop keeps a helper invocation from spinning on what its atomics read, which is undefined ([why](why/emitting.md#emit-162)).
  MSL's loop tests `simd_is_helper_thread()` first, so a helper never enters it.
  HLSL's has no loop, and WGSL cannot tell a helper (EMIT-163).
  A compute and a ray-tracing stage have no helpers, so their loop has no such test.
* **EMIT-163** A pixel stage that reaches `compare_exchange_strong` needs `helper_lane_query`, which the check pass judges on every target as any feature (CHK-322).
  WGSL has no test for a helper invocation, so no loop it holds could keep one out, and EMIT-109 refuses the stage by naming the feature ([why](why/emitting.md#emit-163)).
  A weak compare-exchange is one call, which a helper leaves at once, and a compute stage has no helpers, so neither needs the feature.
* **EMIT-131** A barrier is the target's own execution barrier with one kind of memory, and MSL's one `threadgroup_barrier` names it by its `mem_flags`.
  `workgroup_barrier` is `GroupMemoryBarrierWithGroupSync` in HLSL, `workgroupBarrier` in WGSL and `mem_threadgroup` in MSL.
  `storage_barrier` is `DeviceMemoryBarrierWithGroupSync`, `storageBarrier` and `mem_device`.
  `texture_barrier` is `DeviceMemoryBarrierWithGroupSync`, `textureBarrier` and `mem_texture`.
* **EMIT-121** A binding array is its element's declaration with the length after the name, at its first slot, and the resources after it start `count` slots later.
  HLSL wraps an index marked `nonuniform` in `NonUniformResourceIndex`, which DXC carries into SPIR-V as `NonUniform`.
  MSL needs no mark, and every target writes an index marked `uniform` as the plain index (CHK-400).

| SGL | HLSL | WGSL | MSL |
|---|---|---|---|
| `float` | `float` | `f32` | `float` |
| `float3`, `vec3`, `pos3` | `float3` | `vec3f` | `float3` |
| `float4`, `hpos4` | `float4` | `vec4f` | `float4` |
| `mat4` | `float4x4` | `mat4x4f` | `float4x4` |
| `mat3` | `float3x3` | `mat3x3f` | `float3x3` |
| `int` | `int` | `i32` | `int` |
| `bool` | `bool` | `bool` | `bool` |
| `half`, `half3` | `float16_t`, `float16_t3` | `f16`, `vec3h` | `half`, `half3` |
| `short`, `ushort` | `int16_t`, `uint16_t` | none: WGSL lacks `shader_int16` | `short`, `ushort` |

* **EMIT-140** HLSL writes a 16-bit float as `float16_t` and never as `half`, which without `-enable-16bit-types` silently means a 32-bit float (EMIT-4).
  A literal of a 16-bit type is a construction of its type around it on every target, `float16_t(0.5)`, `f16(0.5)` and `half(0.5)`.
  MSL's bare `0.5` is a 32-bit float, and would widen the expression it stands in.
* **EMIT-190** HLSL writes a 64-bit integer as `int64_t` and `uint64_t` and a double as `double`, MSL as `long` and `ulong`.
  A 64-bit literal carries its width's suffix, `5LL` and `5ULL` in HLSL and `5L` and `5UL` in MSL, so no target reads it as 32 bits; the most negative long is `(-9223372036854775807LL - 1LL)`.
  A double literal is suffixed `L` in HLSL, whose bare `0.1` is a float rounded to 24 bits.
  A shift of a 64-bit value masks a count that is no literal to its low six bits, which HLSL and MSL leave undefined past 63.
* **EMIT-191** A constant block's array of one value per row (EMIT-172) is one of two-vectors for a 64-bit scalar, `int64_t2` in HLSL, since a 16-byte row holds two.
* **EMIT-192** `msl` refuses an entry point that needs `shader_f64`, `shader_int64_atomics` or `shader_shared_int64_atomics` as `target-lacks-feature`, as EMIT-109 says.
  `shader_int64_min_max_atomics` alone it writes (EMIT-218).
  MSL has no double, and its 64-bit atomics are a ulong's min and max alone.
* **EMIT-141** WGSL enables what its text uses ahead of every declaration: `enable f16;` where it names a 16-bit float, and `enable subgroups;` where it holds a subgroup operation or stage input.

## Enums

* **EMIT-76** An enum is one named constant per case, minted as `<enum>_<case>`, of the target's `int`, and the constants stand in front of the structs.
* **EMIT-108** A `@builtin` enum is none of that: it is written as its record spells it, and a case of `bool` is the literal `false` or `true`.
* **EMIT-77** An entry point writes the whole constant set of every enum it mentions, in declaration order ([why](why/emitting.md#emit-77)).
* **EMIT-81** An enum member of an edge struct or of an `@inline` binding is `unsupported`, as `int` and `bool` are by EMIT-33 and EMIT-39.

```hlsl
static const int light_kind_point = 0;
static const int light_kind_spot = 1;
static const int light_kind_sun = 2;
```

```wgsl
const light_kind_point: i32 = 0;
const light_kind_spot: i32 = 1;
const light_kind_sun: i32 = 2;
```

## Addresses

* **EMIT-24** The struct of an entry point's parameter and the struct of its result are its **edge structs**.
* **EMIT-25** What an edge struct is comes from where it stands in the signature, by the table below.
* **EMIT-26** Member order is the address: the **location** of a member is its position among the members that carry no `@position`.
* **EMIT-27** A member with `@position` is `SV_Position` in HLSL and `@builtin(position)` in WGSL, and it takes no location.
* **EMIT-28** A member of a vertex input at location i has the dx12 semantic of its name in upper case, `[[vk::location(i)]]` in vulkan, and `@location(i)` in WGSL.
  A name that ends in a digit gets a `_` after it, `uv1` -> `UV1_`, since HLSL reads a trailing number as the semantic's index.
  A semantic an earlier member of the struct took, ignoring case, gets a `_` after it until it is its own: `uv1` then `uv1_` is `UV1_` then `UV1__`.
  `sgl describe` states each member's semantic, which is what the host's input layout names the member by.
* **EMIT-29** A member of a stage link at location i has the generated semantic `SGLi`, `[[vk::location(i)]]` in vulkan, and `@location(i)` in WGSL.
* **EMIT-30** A member of a render target struct at location i is `SV_Targeti` in HLSL and `@location(i)` in WGSL.
* **EMIT-235** A DXIL signature is packed as DXC's default `-pack-prefix-stable` packs it, so a stage of SGL's own DXIL links against a stage DXC compiled from the same plan, in either direction.
  An element takes the first row, and the lowest free columns, that hold it, in declared order: a struct's members, then the stage inputs.
  A row holds one interpolation mode, and an ordinary element never follows a system one in it.
  A vertex input takes a row of its own, a render target the row of its index, and `SV_TessFactor` and `SV_InsideTessFactor` their fixed places.
* **EMIT-31** A semantic is made from the name as the program writes it, whatever EMIT-18 made of the member.
* **EMIT-32** A vertex input member whose semantic would start with `SV_` is `system-value-semantic`.
* **EMIT-33** A member of an edge struct is of a builtin type whose record says it crosses an edge, which `mat4`, `int` and `bool` do not, or it is `unsupported`.
* **EMIT-34** `@position` anywhere but in a stage link, a second `@position`, and one struct as both edges are `unsupported`.
* **EMIT-91** `@per_instance` and `@stream` on a member of anything but a vertex input are `unsupported`.
* **EMIT-92** A vertex input member's **stream** is the name `@stream` gives it, else `per_instance` where it carries `@per_instance`, else `per_vertex`.
* **EMIT-93** The members of one stream agree on `@per_instance`, or the struct is `unsupported`.
* **EMIT-94** A stream changes nothing in the text: it is which buffer the host reads a member from, and a location stays the member's position ([why](why/emitting.md#emit-94)).
* **EMIT-95** The text of an entry point comes with the pair of every resource and constant buffer it declares, samplers included.
  The pair is the name it minted, and the host name CHK-171 gives ([why](why/emitting.md#emit-95)).

| stage | parameter | result |
|---|---|---|
| `@vertex` | vertex input | stage link |
| `@tessellation_control` | an array of stage links, the patch | patch constants |
| `@tessellation_evaluation` | the patch, and the patch constants | stage link |
| `@geometry` | an array of stage links, and the stream's element as a stage link | none |
| `@pixel` | stage link | render targets |

* **EMIT-122** `msl` refuses a geometry and a tessellation entry point as `target-lacks-feature`, naming the stage's feature as EMIT-109 says.
  Metal has neither stage, and tessellates by a compute kernel instead.
* **EMIT-205** HLSL writes a mesh stage as `[outputtopology("triangle")]` and `[numthreads(x, y, z)]` over `void`.
  Its output `o` is two parameters, `out vertices T o_vertices[N]` and `out indices uint3 o_triangles[M]`.
  `set_counts` is `SetMeshOutputCounts`, and `vertex` and `triangle` are writes to those arrays.
  A mesh stage of `lines` is `[outputtopology("line")]` over `out indices uint2 o_lines[M]`, which `line` writes.
* **EMIT-207** HLSL writes a task stage as `[numthreads(x, y, z)]` over `void`, and declares `groupshared P sgl_payload` for its payload's struct `P`.
  `dispatch_mesh` stores its payload there and calls `DispatchMesh(x, y, z, sgl_payload)`, since HLSL hands a payload over from groupshared memory alone.
* **EMIT-208** A mesh stage's payload parameter is `in payload P p`.
* **EMIT-209** A mesh output's `F` is a third array, `out primitives F o_primitives[M]`, and `primitive` writes to it.
  `F`'s locations follow the vertex struct's, in the one space the pixel stage reads both from, and the pixel stage takes it as a parameter of its own.
  There its members are `nointerpolation`; for vulkan each also carries SPIR-V's `PerPrimitiveEXT`, as `[[vk::ext_decorate(5271)]]`.
  The entry point then declares `SPV_EXT_mesh_shader` and its `MeshShadingEXT` capability, which DXC leaves out of a pixel stage.
* **EMIT-206** MSL writes a mesh entry point as a `[[mesh]]` function of a `metal::mesh<V, F, N, M, topology>` output, F `void` where nothing is written per primitive.
  It writes a task entry point as an `[[object]]` function; WGSL refuses both by EMIT-109.
* **EMIT-261** WGSL refuses a pixel entry point that takes a mesh stage's per-primitive struct (CHK-489) as `target-lacks-feature`, naming `mesh_shader`.
* **EMIT-262** MSL states its entry point's math mode above its includes: `#pragma METAL fp math_mode(fast)` at `.fast`, and `#pragma METAL fp math_mode(safe)` at any stricter level (EMIT-185).
  The Metal compiler takes the text's pragma over its own `-fmetal-math-mode` and `MTLCompileOptions::mathMode`, so a metallib and a source compile keep the same level whatever they were handed.
  A compile of any other MSL keeps the safe math sg and ssc::msl hand it.
  Only the pipeline of a polyfilled mesh stage hands it one there, and that pipeline's pixel stage is written in the form CHK-524 gives it, which takes no such struct.
  Metal counts the primitives alone, so `set_counts` is `set_primitive_count` of its second argument, and `triangle` and `line` set one index per corner.
  The object stage writes its payload and its grid of mesh workgroups from its first invocation, and a pixel stage's per-primitive struct arrives in one `[[stage_in]]` beside the link.
* **EMIT-123** HLSL writes the stages as dx12 and vulkan both take them:
  * a geometry stage is `[maxvertexcount(N)]` over `void`, taking `triangle T tri[3]` and last `inout TriangleStream<T>`, and `emit` and `end_strip` are `Append` and `RestartStrip`;
  * a control stage is a passthrough hull function that returns `patch[point_index]`, named by its `domain`, `partitioning`, `outputtopology`, `outputcontrolpoints` and `patchconstantfunc`;
  * the SGL body is that patch-constant function, `<name>_patch`, taking the `InputPatch`;
  * a stated `max_factor` (CHK-304) is `[maxtessfactor(N)]`, and every return of the body caps each factor at it;
  * an evaluation stage is `[domain(…)]` over the function, taking the `const OutputPatch`, the patch constants, and `SV_DomainLocation`;
  * in patch constants, `@edge_factors` is `SV_TessFactor` and `@inside_factors` `SV_InsideTessFactor`;
  * the other members of patch constants take the locations after those the control point's members and pieces take, so vulkan's two never collide.
* **EMIT-134** A control stage's `.counter_clockwise` is HLSL's `triangle_cw` and `.clockwise` its `triangle_ccw`.
  HLSL names the winding in its domain's own orientation, which mirrors the patch the domain location weighs (CHK-304).

```hlsl
struct pixel_input
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 normal : SGL0;
    [[vk::location(1)]] float3 color : SGL1;
};
```

## Bindings

* **EMIT-35** The bindings an emitter writes are the ones the entry point's binding list names.
* **EMIT-36** An `@inline binding` is a struct of its members and one global of that struct, which has the binding's name.
* **EMIT-37** The global is where `sg` expects inline constants, by the table below.
* **EMIT-38** A second `@inline` binding is `unsupported`, and so is one a group of the list follows.
  `@workgroup` memory is bound by no host, so it may stand on either side of it.
* **EMIT-39** A plain member of a binding, and a buffer's element, is a value that can stand in GPU memory, or it is `unsupported`.
  That is a builtin whose record has a size there, or a struct of such values; `bool` has no size, and the detail names `bool32`, which has one.
* **EMIT-40** Every value in GPU memory is placed by [the layout rules](#layout), and `hlsl-vulkan` states each offset as `[[vk::offset]]` ([why](why/emitting.md#emit-40)).
* **EMIT-41** Retired: every target is made to follow the layout rules (EMIT-112), so no offset differs between them, and `layout-mismatch` is reported by nothing.

| target | the global |
|---|---|
| `hlsl-dx12` | `ConstantBuffer<T> name : register(b0, space9);` |
| `hlsl-vulkan` | `[[vk::push_constant]] ConstantBuffer<T> name;` |
| `wgsl` | `@group(3) @binding(0) var<uniform> name: T;` |
| `msl` | no global: the parameter `constant T& name [[buffer(4)]]` (EMIT-58) |

A binding that is not `@inline` is a group.

* **EMIT-82** A group's number is its position in the entry point's binding list, the `@inline` binding skipped.
* **EMIT-83** A group's plain members are a struct of their own and one constant buffer of it, named after the binding, at slot 0.
* **EMIT-84** A group's plain members are placed by EMIT-39 and EMIT-40, as the members of an `@inline` binding are.
* **EMIT-85** A buffer member is one global whose name is minted from `<binding>_<member>`, and a group's constant buffer is named after the binding, as any declaration is.
  A second binding of that name in the list, another module's, is minted a name of its own, `frame_1`.
* **EMIT-86** HLSL writes each declaration of a group at file scope, under the name EMIT-85 minted for it, and each carries its address by EMIT-104 ([why](why/emitting.md#emit-86)).
* **EMIT-87** The struct of a group's constant buffer stands ahead of the group's declarations ([why](why/emitting.md#emit-87)).
  `hlsl-vulkan` states every member's offset on it, as EMIT-40 says.
* **EMIT-88** WGSL writes each resource of a group as `@group(N) @binding(slot)`: the constant buffer as `var<uniform>`, a buffer as a `var<storage>` array, `read` or `read_write`.
* **EMIT-89** MSL writes a group as an argument buffer: a struct minted from `<binding>_arguments`, whose member `[[id(n)]]` is slot n of the group.
  The entry point takes it as `constant T& <binding>_group [[buffer(N)]]` for group N, and [bindings.md](../bindings.md#how-a-group-reaches-sg) shows one.
  The constant block is a pointer at slot 0, and a binding array is a C array over as many ids as it has elements.
  The body reads each resource and the block through a local the top of the function binds under the name EMIT-85 minted, so it reads as the other targets' does.
* **EMIT-90** A group's resources — buffers, textures, images and samplers — take the slots after its constant buffer, in declaration order, from 1, or from 0 when it has no plain member.
* **EMIT-97** A texture, an image and a sampler member are each one global minted as a buffer's is, by EMIT-85, and each has its target's own type by the table below.
* **EMIT-98** `hlsl-vulkan` states an image's format as `[[vk::image_format]]`, in DXC's spelling, which DXC makes a typed image of.
  `hlsl-dx12` states none, since dx12 takes the format from the view, and neither does `bgra8_unorm` on vulkan, which SPIR-V has no name for.
* **EMIT-99** A static sampler of a group is its `SamplerState`, or `SamplerComparisonState` where it has a `compare`, with its address and nothing else.
  Its settings reach the layout from `sgl describe`, never from the text, so every target writes it as it writes a sampler the host binds.
* **EMIT-100** WGSL and MSL write a 1D texture or image as a 2D one and a 1D array as a 2D array, since sg's webgpu and metal backends create every 1D texture that way (the bindings file, "Shapes").
* **EMIT-101** A call of a builtin that gives nothing is a statement as it stands, with no `_ =` in WGSL.
* **EMIT-102** A builtin a target cannot write as one expression declares a helper function ahead of the entry point, once per text, and the call names it.
  HLSL's `GetDimensions` writes through out parameters, so `size` is an overload of `sgl_size` per texture type the entry point passes.
* **EMIT-103** WGSL text that takes a derivative opens with `diagnostic(off, derivative_uniformity);`, and text that enables `subgroups` with `diagnostic(off, subgroup_uniformity);`.
  The check pass has judged both already, by rules that reconverge after a loop where Tint's do not (CHK-284), so Tint's verdict would only refuse what the pass accepted.
  A barrier has no such switch in WGSL, which is why the pass judges a barrier by WGSL's rules.
* **EMIT-104** A resource at slot i of group N is `register(<class>i, spaceN)` in `hlsl-dx12` and `[[vk::binding(i, N)]]` in `hlsl-vulkan`, which is the address sg's backends give that slot.
  The class is `b` for the constant buffer, `u` for an image and a `mut` buffer, `s` for a sampler, and `t` for every other resource.
* **EMIT-105** An entry point that lists more than three groups is `too-many-groups` on every target, since sg binds three besides the inline constants.
  Only the entry point's list counts: a function that is no entry point takes the groups its caller hands it, which are no addresses of their own.
* **EMIT-133** A file-scope sampler the entry point's code reaches is one global named as the sampler is, and one the code does not reach is not declared.
  Its index i is its position among every file-scope sampler the program and the modules it uses declare, in declaration order with the modules' files first.
  So every entry point and every stage states the same one, and a module's is described to the host beside the program's, by its qualified name.
  It stands where sg binds a pipeline layout's static sampler of index i: `register(s<i>, space10)` in `hlsl-dx12`, and `[[vk::binding(i + 1, 3)]]` in `hlsl-vulkan`.
  WGSL writes it as `@group(3) @binding(i + 1)`, since binding 0 of sg's own group is the inline constants', and MSL as the entry point's parameter `sampler name [[sampler(i)]]`.
  A ray-tracing stage's is a `constexpr sampler` of its settings in MSL instead, since a visible or an intersection function has no sampler slot to read.
  A `mip_lod_bias` other than 0 is `unsupported` there, since a `constexpr sampler` has no bias.
  Its type is EMIT-99's, and its settings reach the layout from `sgl describe` as a group's static sampler's do.
  An entry point that reaches one of index 16 or more is `too-many-samplers` on every target, since Metal's argument table and WebGPU's default `maxSamplersPerShaderStage` hold 16.
  Every sampler declared above it counts toward that index, reached or not, so the limit is on the file and not on what one stage uses.
  So a library file holding more than 16 samplers splits into several, or whatever reaches its seventeenth is refused.

| SGL | HLSL | WGSL | MSL |
|---|---|---|---|
| `texture_2d[float4]` | `Texture2D<float4>` | `texture_2d<f32>` | `texture2d<float>` |
| `texture_2d_depth` | `Texture2D<float>` | `texture_depth_2d` | `depth2d<float>` |
| `image_2d[.rgba8_unorm]` | `RWTexture2D<float4>` | `texture_storage_2d<rgba8unorm, read>` | `texture2d<float, access::read>` |
| `out image_2d[.r32_float]` | `RWTexture2D<float>` | `texture_storage_2d<r32float, write>` | `texture2d<float, access::write>` |
| `sampler`, `comparison_sampler` | `SamplerState`, `SamplerComparisonState` | `sampler`, `sampler_comparison` | `sampler`, `sampler` |

The other shapes follow the same pattern: HLSL's `Texture2DArray`, `TextureCube`, `Texture2DMS`, and WGSL's `texture_2d_array`, `texture_cube`, `texture_multisampled_2d`.
MSL's are `texture2d_array`, `texturecube` and `texture2d_ms`.
MSL's texture takes the scalar it holds rather than the vector, and a buffer is `device T*`, `const` where the shader only reads it.
An image's HLSL element is the texel of its format, one to four wide, and a load gives that; WGSL always loads and stores four channels, so its writer narrows a load and pads a store.

```sgl
binding affine:
    scale: float
    bias: float
    values: mut buffer[float]

@compute(64) fun affine_map(@thread_id id: int3){affine}:
    affine.values[id.x] = affine.values[id.x] * affine.scale + affine.bias
```

* **EMIT-155** `bytes` is `ByteAddressBuffer` in HLSL and `RWByteAddressBuffer` where it is `mut`, a `var<storage>` array of `u32` in WGSL, and a `device uint*` in MSL.
  A call is HLSL's `Load`, `Load2` to `Load4` and `Store` to `Store4`, and elsewhere words of the array at the offset over 4, the index of several bound once.

## Matrices

* **EMIT-42** A matrix is column-major, and a vector stands to its right.
* **EMIT-43** HLSL declares every matrix member `column_major` and writes a product `mul(a, b)`; WGSL writes `a * b`, for a matrix times a vector and for a matrix times a matrix.
* **EMIT-44** `transform_position(m, p)` is the product of `m` and the four-vector `(p, 1.0)`.
* **EMIT-45** `transform_direction(m, v)` is the `xyz` of the product of `m` and `(v, 0.0)`.
* **EMIT-166** `m * h` of a `mat4` and an `hpos4` is their product, an `hpos4`, so a chain of projective matrices keeps the type a position took on its first.
* **EMIT-156** `mat4(c0, c1, c2, c3)` is the matrix of those columns: WGSL's and MSL's constructor of columns, and HLSL's constructor of rows under a `transpose`.
* **EMIT-160** `mat3(c0, c1, c2)` is `transpose(float3x3(c0, c1, c2))` in HLSL, whose constructor takes rows, and the target's own constructor of three columns in WGSL and MSL.
  A product of a `mat3` is written as EMIT-43 writes a `mat4`'s, and no memory holds one, so no declaration of a block or a buffer names it (CHK-397).

## The text

* **EMIT-46** The text is meant to be read: real names, one statement per line, four spaces per level ([why](why/emitting.md#emit-46)).
* **EMIT-47** The text starts with a comment that names the stage, the entry point and the target.
* **EMIT-48** An immutable local is a named constant: `const T name = value;` in HLSL and `let name: T = value;` in WGSL.
* **EMIT-180** An inlined call's argument is bound to a local unless it is a literal or an immutable local, and that local is written into its one read where the body reads it once.
  Only where the read stands outside every loop, and the argument is stable: literals, immutable locals and pure operations over them, which nothing the body writes can change.
  So `max(v, 0.0)` of a generic reads as the vector call it inlines to, `max(a * b, float3(0.0))`, rather than binding `a * b` first.
* **EMIT-49** A float literal is the shortest decimal text that reads back as its value, always with a decimal point, and without a suffix.
* **EMIT-50** A literal that is infinite or not a number is `non-finite-literal`.
* **EMIT-51** An `@operator` builtin is its operator: `+`, `-`, `*`, `/`, `%`, the six comparisons, and the prefix `-`; the products of a matrix are EMIT-43.
  Parentheses follow the tree, and an operand of equal precedence on the right keeps them.
* **EMIT-125** An operand of `&`, `|`, `^`, `<<` or `>>` is parenthesized unless it is a name, a call, a literal or a prefix expression, since WGSL takes nothing else there.
  The operation itself is parenthesized wherever it is embedded.
  WGSL writes an `int` count as `u32(count)`, and MSL masks the count, `x << (count & 31)`, since a count past 31 is undefined there.
* **EMIT-126** A maths builtin is the target's function of its name, with these exceptions.
  `inverse_sqrt` is `rsqrt` in HLSL and MSL and `inverseSqrt` in WGSL; `fract` is `frac` in HLSL; `round` is `rint` in MSL, whose `round` is ties-away.
  `ddx` and `ddy` are coarse (EVAL-144): `ddx_coarse` and `ddy_coarse` in HLSL and `dpdxCoarse` and `dpdyCoarse` in WGSL, since the plain names leave coarse or fine to the implementation.
  `fwidth` is `sgl_fwidth_coarse`, a helper over those, in HLSL and `fwidthCoarse` in WGSL; `ddx_fine` and `ddy_fine` are `ddx_fine` and `ddy_fine` in HLSL and `dpdxFine` and `dpdyFine` in WGSL.
  MSL has no coarse spelling: every derivative is `dfdx`, `dfdy` or `fwidth`, which are fine on Apple GPUs; issue: feat-sgl-coarse-derivatives-by-default.
  HLSL's `sign`, `countbits`, `firstbithigh` and `firstbitlow` give an int or a uint whatever they take, so the writer converts back, and `reversebits` of an int goes through a uint.
  `is_nan` and `is_inf` are a test of the float's bits on every target: `(bits & 0x7fffffff)` above `0x7f800000` is a NaN, and equal to it an infinity.
  No target may be handed the value, since a test of it could fold to false where a test of its bits cannot.
  Vulkan may assume neither arises unless a module declares `SignedZeroInfNanPreserve`, which DXC's SPIR-V does not; and WGSL may assume it outright.
  Metal's own default is fast math, so sg and `ssc::msl` compile MSL in its safe mode, which keeps both.
  A half is tested as the float it widens to, which keeps a NaN and an infinity exactly, since WGSL bitcasts no lone `f16`.
  WGSL writes a reinterpretation as `bitcast<T>`, MSL as `as_type<T>` and HLSL as `asuint`, `asint` or `asfloat`.
  MSL's `first_bit_*` and HLSL's packing functions are helpers the text declares (EMIT-102).
* **EMIT-127** A stage input is a parameter of the target's entry point, handed over as the target's own type, and converted to SGL's where the body reads it.
  HLSL marks it with a semantic (`SV_VertexID`, `SV_DispatchThreadID`, …), WGSL with `@builtin(…)` and MSL with an attribute (`[[vertex_id]]`, …).
  One the body never reads stays a parameter, and has no line converting it.
  WGSL's `primitive_index` is an extension, which the text enables ahead of every declaration.
* **EMIT-128** HLSL counts a vertex and an instance from the draw's base, on dx12 and on vulkan through DXC alike, so the text adds `SV_StartVertexLocation` and `SV_StartInstanceLocation` back.
* **EMIT-129** A stage link's member carries its interpolation: HLSL's `nointerpolation`, `noperspective`, `centroid` and `sample`, and WGSL's `@interpolate(…)`.
  MSL names each combination as one attribute: `flat`, `centroid_perspective`, `center_no_perspective` and their like.
  The default, perspective at the centre, is written by none of them.
* **EMIT-130** A `@depth` member is `SV_Depth` in HLSL, `@builtin(frag_depth)` in WGSL and `[[depth(any)]]` in MSL.
  A `@sample_mask` member is `SV_Coverage`, `@builtin(sample_mask)` and `[[sample_mask]]`.
  The conservative forms are `SV_DepthGreaterEqual` / `SV_DepthLessEqual` and `[[depth(greater)]]` / `[[depth(less)]]`; WGSL has none, and drops the promise, which changes no result.
  On dx12 a pixel stage writing `.greater_equal` or `.less_equal` takes its `@position` as `noperspective centroid`, which DXIL requires of one.
* **EMIT-117** A `discard` is `discard;` in HLSL and WGSL, and `discard_fragment();` in MSL.
* **EMIT-124** A float `%` is `fmod(a, b)` in MSL, which has no `%` of floats; HLSL's and WGSL's `%` of floats already mean EVAL-83's remainder.
* **EMIT-263** A signed integer `%` is `a - (a / b) * b` wherever it becomes SPIR-V, since Vulkan leaves `OpSRem` undefined for a negative operand and defines `OpSDiv`.
  The native route writes those three operations, and HLSL for vulkan calls the helper `sgl_srem<T>`; every other target's `%` already means EVAL-83's remainder.
* **EMIT-52** Every other builtin function is a call of the target's function of that name, and `mix` is `lerp` in HLSL, with a scalar `t` or a vector one alike.
* **EMIT-53** A construction of a builtin type is a call of the target's type, on one line: `float3(x, y, z)`, `vec3f(x, y, z)`.
* **EMIT-157** Arguments of a vector's construction that are every field of one vector local, in order, are that local whole: `float4(..lit, 1.0)` is `float4(lit, 1.0)`.
* **EMIT-73** The operand of a prefix `-` that is no name, call or member stands in parentheses, so `-(-0.4)` never reads as a decrement.
* **EMIT-74** How a builtin is written is a field of its registry record: a call under a name per target, an infix or a prefix operator, or a writer of its own ([why](why/emitting.md#emit-74)).
  No emitter holds a list of builtins, and the size and alignment the layout rules and each target's own rule place a value by are fields of the type's record.
* **EMIT-75** A value that is evaluated and dropped is a statement of its own: `value;` in HLSL, `_ = value;` in WGSL, and `(void)(value);` in MSL ([why](why/emitting.md#emit-75)).
* **EMIT-54** A construction of a struct of the program is `name(a, b)` in WGSL.
* **EMIT-55** In HLSL it is a local that is declared and then assigned member by member; a returned one is minted from `result` ([why](why/emitting.md#emit-55)).
* **EMIT-68** Control flow is written by [the table of the core form](legalization.md#the-table), and a nested body is one level deeper.
* **EMIT-69** The C-like targets put a brace on a line of its own and a condition in parentheses; WGSL puts the brace behind the head and writes the condition bare.
* **EMIT-70** An `int` literal is its decimal text, and the one that does not fit behind a minus is `(-2147483647 - 1)`.
* **EMIT-158** An integer literal or `const` written in hex stays hex, in lowercase and without its `'` separators, since a bit pattern reads as one only there.
  A negative `int` is the exception and keeps its decimal text, as does a value an operator over literals folded to.
* **EMIT-71** A `while` whose condition builds a struct member by member is written as a loop that tests at its top, so the struct is built before every test.
  So is one whose condition calls a builtin the target writes as statements of its own, such as HLSL's atomics or the uniform load of EMIT-149 outside WGSL.
* **EMIT-78** A `switch` is the target's own, by the table of the core form, and a label is the constant of EMIT-76 where its value is a case and its decimal text otherwise.
* **EMIT-79** In the C-like targets the emitter ends each arm with `break;`, and writes none after a body that already exits.
* **EMIT-80** WGSL writes an arm's values as one comma list, and the C-like targets as one label per value.
* **EMIT-142** A swizzle of a prelude vector is the target's own, `v.zyx` on every target, since every prelude vector is a vector of every target ([why](why/emitting.md#emit-142)).
  A swizzle of a struct of the program is a construction of its vector, one member access per letter, with an operand that is no local bound to a temporary first, as CHK-103 binds one.
  A splat of a swizzle of a local is one member access of it per letter, so `float4(..v.xyz, 1.0)` binds no temporary.
* **EMIT-143** HLSL and MSL assign through a swizzle of a prelude vector as it stands, `v.xz = v.xz + d;`.
  WGSL assigns one component at a time, and so does every target through a swizzle of a struct of the program.
  The new value goes into a temporary first, which reads the old value and the right side once, and each component is stored from it.
* **EMIT-144** `==` and `!=` over two vectors are `all(a == b)` and `any(a != b)` on every target, and `equal` and `not_equal` are the targets' componentwise `==` and `!=`.
  An operator between a vector and its scalar, a one-value constructor, `any`, `all` and `fwidth` are each the target's own.
  HLSL's constructor takes every component, so its one-value constructor is a cast, `(float3)x`.
  Where a target takes no scalar beside a vector, the scalar is written as that vector: WGSL's `&`, `|`, `^` and shifts, and MSL's `fmod`.
* **EMIT-145** `select(cond, if_true, if_false)` is `select(cond, if_true, if_false)` in HLSL, and `select(if_false, if_true, cond)` in WGSL and MSL.
  MSL takes a `bool` condition beside scalars alone, so one beside vectors is spread, `select(f, t, bool3(c))`.
  Where that order would evaluate an argument with an effect ahead of one SGL evaluates first, the arguments are bound to locals in SGL's order.

In WGSL, `v.xz += d` over a `float4` local `v` and a `float2` `d` reads so:

```wgsl
let sgl_t: vec2f = v.xz + d;
v.x = sgl_t.x;
v.z = sgl_t.y;
```

```hlsl
pixel_input main_vs(cube_vertex v)
{
    pixel_input result;
    result.position = mul(constants.view_projection, float4(v.position, 1.0));
    result.normal = v.normal;
    result.color = v.color;
    return result;
}
```

```wgsl
@fragment
fn main_ps(p: pixel_input) -> target_ {
    let n: vec3f = normalize(p.normal);
    let key: f32 = saturate(dot(n, normalize(vec3f(0.45, 0.8, -0.4))));
    let fill: f32 = saturate(dot(n, normalize(vec3f(-0.7, 0.15, 0.6))));
    let lit: vec3f = p.color * (0.25 + 0.8 * key + 0.25 * fill);
    return target_(vec4f(lit.x, lit.y, lit.z, 1.0));
}
```

## SPIR-V

* **EMIT-232** Where a compile asks, `hlsl-vulkan` is written as a SPIR-V 1.6 module by SGL itself rather than as text for DXC: `text_request::native`, `sgl emit --target spirv`.
  The plan is the text's, so every name, address and layout the host reads is the same on either route.
  `require` makes an entry point with no lowering yet the error `unsupported`, naming what it holds, and `prefer` writes the text and says why in `emitted_source::native_gap`.
  Every stage lowers, and what does not yet is named by [feat-sgl-native-compute-gaps](../../issues/feat-sgl-native-compute-gaps.md); a target's own text never does.
  Float `!=` is unordered, a shift's count is masked to its width, and `and` and `or` branch where their right side could go wrong; nothing here widens what a program may do.
* **EMIT-233** A float operation of an entry point past `fast` is decorated `NoContraction`, and the entry point states `SignedZeroInfNanPreserve` per float width, and `DenormPreserve` for `ieee`.
  This is what DXC's `-Gis` and `-denorm preserve` write, per operation in the layer, so a guarantee narrower than an entry point needs no change to the form.
* **EMIT-234** Where a compile asks, `hlsl-dx12` is written as a DXIL container by SGL itself: `text_request::native`, `sgl emit --target dxil`.
  It takes what EMIT-232 says of a plan, of `require` and `prefer`, and of what a program may do.
  The container states shader model 6.6, or 6.8 where EMIT-128 reads a start location, and carries the retail bypass digest, which is what lets D3D12 load a shader no validator signed.
  A `fast` float operation carries LLVM's fast flags, and one past it carries none, which is what `-Gis` writes; only a `fast` addition may reorder.
  `ieee` states `fp32-denorm-mode` on the entry point, as `-denorm preserve` does.
* **EMIT-236** A vertex or pixel entry point of a native route reads each element of its edge ahead of its body, and each `return` stores each output element.
  So a stage of either route links against a stage of the text route, in both directions.
  A pixel stage's `@position.w` on SPIR-V is the reciprocal of `FragCoord.w`, as `-fvk-use-dx-position-w` writes it.
  `@vertex_index` and `@instance_index` add the draw's base on DXIL alone (EMIT-128), since `VertexIndex` and `InstanceIndex` already hold it.
  A 64-bit link crosses as its words on DXIL (EMIT-196) and as itself on SPIR-V, and a pixel stage's second struct is what a mesh stage wrote per primitive (CHK-489), `PerPrimitiveEXT` on SPIR-V.
  A link of 16-bit numbers is `unsupported` so far.
* **EMIT-237** DXIL's `dx.viewIdState` and `PSV0` state that every declared input lane reaches every declared output lane.
  A table may say too much and never too little, and the validator takes this one.
  A signature's usage masks are read off the module's final operations, so a lane the passes dropped is no use.
  A stage that reads `@view_index` also states that every output lane depends on the view, which may say too much in the same way.
* **EMIT-238** A native route writes a pixel stage's surface as its target's own operations, and `discard` terminates on neither.
  It is `OpDemoteToHelperInvocation` and `dx.op.discard(true)`, and the invocation goes on as a helper, so a neighbour's derivatives stay whole.
  `ddx`, `ddy` and `fwidth` are `OpDPdxCoarse`, `OpDPdyCoarse` and `OpFwidthCoarse` under `DerivativeControl`, and on DXIL `derivCoarseX`, `derivCoarseY` and the sum of their absolute values.
  `ddx_fine` and `ddy_fine` are `OpDPdxFine` and `OpDPdyFine`, and `derivFineX` and `derivFineY`.
  `@depth` is `FragDepth` with `DepthReplacing`, plus `DepthGreater` or `DepthLess`, and DXIL's three depth elements.
  `@sample_mask` is SPIR-V's array of one on both sides of the stage, and on DXIL an output element with no row or `dx.op.coverage`.
  `@sample_index` stands in a signature with no row, and DXIL reads it with `dx.op.sampleIndex`; such an element is unallocated in `PSV0`.
  `@view_index` is `dx.op.viewID` and in no signature, and `@layer` from a vertex stage is `ShaderLayer` and DXIL's shader flag for it.
  Every built-in a pixel stage reads but `FragCoord` is `Flat` on SPIR-V.
  A sample's offset is EMIT-253's.
* **EMIT-239** A binding array is one global of an array of resources, an unbounded one of a runtime array, and an `access_chain` picks the element.
  On DXIL that element's handle is made where the chain stands, `createHandleFromBinding` over the array's whole range, since no handle exists ahead of its index.
  `nonuniform i` marks the chain and what reads through it.
  SPIR-V decorates the index, the chain, the load and the sampled image `NonUniform`, and DXIL sets `createHandleFromBinding`'s last argument.
* **EMIT-240** A geometry or tessellation entry point of a native route keeps the shape of the stage it stands for, so it links against the text route's in either direction.
  A parameter's array stays where the stage holds it: `tri[i]` reads that vertex's elements at `i`, whatever `i` is, and only the array read whole loads every vertex.
  `emit` stores the output elements and is `OpEmitVertex` or `dx.op.emitStream`; `end_strip` is `OpEndPrimitive` or `dx.op.cutStream`.
  A control stage's body is one function, EMIT-123's patch-constant function, and each writer generates the rest from the elements.
  On DXIL the entry point is the function that hands control point `outputControlPointID` on, and the patch-constant function is named in the entry's hull state.
  On SPIR-V the entry point hands point `InvocationId` on, then `OpControlBarrier`, then calls the body where that id is 0.
  A patch's elements are `Patch` on SPIR-V, `storePatchConstant` and `loadPatchConstant` on DXIL, read in an evaluation stage.
  `@edge_factors` and `@inside_factors` are one element per factor.
  SPIR-V holds them in `TessLevelOuter` of four and `TessLevelInner` of two, always that long, and DXIL states each array as one element of as many rows.
  `@domain_location` is `TessCoord`, three lanes on any domain, or `dx.op.domainLocation`, and `@primitive_id` outside a pixel stage is `dx.op.primitiveID`.
  `ISG1`, `OSG1` and `PSG1` list elements by register and then by the offset of the name, so a factor that declares first leads a link standing before it in its row.
  EMIT-237's table reaches across patch constants too: every input to every patch constant in a control stage, and every patch constant to every output in an evaluation stage.
* **EMIT-241** A native route has no `print` of its own: it lowers what LEGAL-70 wrote, so sg reads a binary's records as it reads a text's, and one pipeline may mix the routes.
  `DXIL` states `UAVs at every shader stage` wherever a vertex, geometry or tessellation stage declares a UAV.
* **EMIT-242** A mesh or task entry point of a native route writes what its text writes, in its target's own operations, so it links against the text route's in either direction.
  `set_counts` is `OpSetMeshOutputsEXT` or `dx.op.setMeshOutputCounts`.
  `vertex` and `primitive` store each element at their index, through `storeVertexOutput` and `storePrimitiveOutput` on DXIL and an access chain on SPIR-V.
  `triangle` and `line` are `dx.op.emitIndices`, a line's third index zero, or a store into `PrimitiveTriangleIndicesEXT` or `PrimitiveLineIndicesEXT`, an array no element holds.
  Per-primitive elements are `PerPrimitiveEXT` on SPIR-V, and stand in `PSG1` on DXIL, a part it holds only where it writes any.
  A task stage stores its payload whole in one global, and `dispatch_mesh` ends the stage on both targets: `OpEmitMeshTasksEXT`, or `dx.op.dispatchMesh` and a return.
  A mesh stage reads the payload through that place, so reading an element of an array copies no array.
  On SPIR-V the payload is `TaskPayloadWorkgroupEXT` of the struct itself, and a workgroup of more than one invocation has an `OpControlBarrier` ahead of the dispatch, as DXC writes.
  On DXIL it is a struct of the shape DXC gives it, whatever the route: a vector or a matrix is an array of scalars, a `bool` an `i32`, and each lies at its natural alignment with no 16-byte rule.
  So `float` then `float4` takes 20 bytes, the size the entry's metadata and `PSV0` both state, and a mesh stage reads it with `getMeshPayload`.
* **EMIT-243** An inline trace of a native route holds one query, which its target's text would hold, and reads it by the operations that text compiles to.
  On SPIR-V the query is an `OpVariable` of `Function` at the top of the body, however deep it begins.
  On DXIL it is one `dx.op.allocateRayQuery` where it begins, with the flags every trace of it states.
  A read of a query gives a value and goes where nothing uses it, but it neither moves nor merges, since each is valid only in the state the query is in where it stands.
  A row of a hit's transform is taken from the whole transform, which SPIR-V hands over by column and DXIL by row and column.
  An `affine3` is a value of three rows, and GPU memory holds one as EMIT-259 writes it.
  A query of an entry point that states opacity micromaps is EMIT-184's.
  What is undefined on one way into a merge takes the other way's value, since DXIL's validator refuses `undef` in a `select`.
* **EMIT-244** A ray-tracing stage of a native route is one entry point of its target's own, so a pipeline may take each stage from either route.
  A payload, a callable's parameter and a hit's attributes are each one variable of the stage, and a trace or a call copies into a variable of the caller's, one per type, and back.
  An any hit's decision ends the stage: `OpIgnoreIntersectionKHR` or `OpTerminateRayKHR`, `dx.op.ignoreHit` or `dx.op.acceptHitAndEndSearch`.
  An intersection stores what it reports, then `OpReportIntersectionKHR` or `dx.op.reportHit`, with hit kind 0.
  On SPIR-V what a stage reads of its launch, ray and hit is a `BuiltIn`, and on DXIL an operation of each.
  On DXIL a stage is a library of one export under its own name, its payload and attributes parameters by pointer.
  A resource there is an external handle a function loads, through `createHandleForLib`.
  The container holds `SFI0`, `RDAT` and `DXIL`, and `RDAT` states what the module implies, since the validator compares the two.
  An intersection fused with its any hit is metal's alone (EMIT-139), and `unsupported` here, and so is a closest hit of no payload on DXIL.
  A hit object and a reordering trace are EMIT-246's.
* **EMIT-245** A source a host compiles at run time takes the context's options as a package's shader does: what the device has, its float guarantees and its native route.
  So a generated hit group, a host's callable and a generated kernel lower where the context prefers SGL's binary, and `has` answers for the device.
* **EMIT-246** A hit object of a native route is the value of its trace and nothing else, so EMIT-225's rule holds of the program and not only of its text.
  No local, global or member holds one, and no phi, `select`, load, store or composite moves one, which `ir::verify` refuses after every pass.
  Only `op::hit_object_trace` and `op::trace_reorder_execute` give one; only a read, a regrouping and an invocation take one.
  A `let` of a trace names that value, and a parameter stands for it as EMIT-225 says.
  On SPIR-V it is a `Function` variable of `OpTypeHitObjectEXT` at the top of the body, reached by its pointer, and a trace and an invocation hand their payload over in a `RayPayloadKHR` variable.
  A trace with `reorder` is `OpHitObjectTraceReorderExecuteEXT`, and `maybe_reorder` is `OpReorderThreadWithHintEXT`.
  On DXIL it is a value of `%dx.types.HitObject`, from `hitObject_TraceRay`, at Shader Model 6.9.
  A trace with `reorder` is that, `maybeReorderThread` and `hitObject_Invoke`, and `maybe_reorder` regroups a `hitObject_MakeNop`.
  A regrouping with no hint states an undefined hint of 0 bits, as DXC's does.
* **EMIT-247** A native route's atomic on a 64-bit integer is its target's own, as an `i32` one is.
  On DXIL it is the `i64` form of `dx.op.atomicBinOp` and `dx.op.atomicCompareExchange` on a buffer, and an `atomicrmw` or `cmpxchg` of `i64` in workgroup memory.
  A resource it reaches carries Atomic64Use in its metadata, and `PSV0` flags it `UsedByAtomic64`, since the validator compares the two.
  One in workgroup memory states the shader flag for 64-bit atomics on group-shared memory, and one on a buffer needs nothing past 64-bit integers.
* **EMIT-248** A native route declares LEGAL-69's shadow as one workgroup global `sgl_written` of a `uint` per 32 scalars, and a word of it is that global's element.
  What reads, marks and clears it is LEGAL-71's statements, which it lowers as any other, so neither writer knows of a prologue.
* **EMIT-249** DXIL writes each subgroup and quad operation as the operations HLSL's text compiles to.
  A shuffle by an offset reads `waveReadLaneAt` at the lane it computes from `waveGetLaneIndex`, as EMIT-146's text does.
  An inclusive sum or product is `wavePrefixOp` and then this invocation's own value folded in, since DXIL's prefix is exclusive alone.
  A ballot is `waveActiveBallot`'s four words, a quad swap `quadOp` of its axis, and a quad broadcast `quadReadLaneAt`.
* **EMIT-250** A native route's `atomic[float]` held as a `uint`'s bits is that `uint`'s atomic, bitcast either side, in shared lowering.
  Its add is the text's loop: a compare-exchange of the bits of the sum, retried with what it read until no other update came between.
  The first guess is an atomic load where the text reads plainly, since either is only a guess the exchange checks.
* **EMIT-251** A native route holds a `bool32` as a `uint` of 0 or 1, in memory and as a value alike.
  `x as bool32` is a `select` of 1 and 0, and `x as bool` is a comparison with 0, so any word but 0 reads as `true`, as the text's does.
* **EMIT-252** A native route updates an `@atomic` image's texel through a place of its own, the image and the coordinate, with an array's layer as the coordinate's last lane.
  On SPIR-V it is `OpImageTexelPointer` of the image's variable, sample 0, in `Image` storage, which the atomics take as they take any pointer.
  On DXIL it is `atomicBinOp` or `atomicCompareExchange` of the image's handle, the coordinate's lanes as its offsets and `undef` past them.
* **EMIT-253** A native route takes a `sample` or a `gather` offset as the constant CHK-280 makes it.
  On SPIR-V it is the `ConstOffset` image operand of an `OpConstantComposite`, and on DXIL the immediate offsets of `sample` and `textureGather`, `undef` past its lanes.

## Lines

* **EMIT-181** Each line of the function body maps to the source line it writes, and to every call it was inlined through, innermost first.
  A line belongs to the statement being written, so a brace, an `else` and a let the emitter hoisted map to their statement's line.
  The prelude's frames are left out wherever another frame remains: a builtin written in SGL maps to the line that called it, which is a line its author wrote.
  The table is the emitter's result, written when the caller hands it the source files; a line is counted as the text holds it, directives included.
* **EMIT-182** Asked for directives, HLSL and MSL write `#line n "file"` before a statement's first line wherever the compiler would otherwise take it for another line.
  So the target's compiler and debugger name the SGL line themselves, and a run of statements on consecutive lines takes one directive.
  Where the inlined calls change, a comment names them first, `// inlined from shade.sgl:12 > main.sgl:40`, each file by its name alone.
  The file name is a string literal, so a backslash or a quote in it is escaped.
  A body in a target's own text names its own first line, and keeps its empty lines, so the compiler names the `.sgl` line of an error inside it.
  After the text, a directive names the statement's line again.
  WGSL has no such directive, and its messages map back through the table (EMIT-181).
* **EMIT-257** Asked for a native line table, a native route writes each operation's line of EMIT-181 into its binary, and nothing else changes.
  It is opt-in, `text_request::native_line_table`, since it carries the text of each file a line names.
  SPIR-V states an `OpString` and an `OpSource` with the text per file, and an `OpLine` before an operation whose line differs from the last in its block.
  Core SPIR-V has no inlined calls, so an `OpLine` names the innermost line, where the operation stands.
  The files' ids are the module's last, so every other id is the one it has without the table.
  DXIL keeps its `DXIL` part byte for byte and writes DXC's `-Zi` form beside it: an `ILDB` part and an `ILDN` part naming `<entry>.pdb`.
  `ILDB` is the module again, each instruction with a `DILocation`, an inlined statement's in a `DISubprogram` of its file with the chain of `inlinedAt` calls.
  It holds `dx.source.contents`, so a debugger shows each line without the file, and no variable is described.
  A function's expression inlines into the statement that calls it, so only a function's own statements keep an inlined line.

## Diagnostics

* **EMIT-183** Asked for diagnostics, a compile writes each `print` into sg's debug window as one record, and describes its site to the host.
  No target and no route holds this: the tree must keep its prints and lower its asserts (LEGAL-64), LEGAL-70 writes them as statements, and a target writes those like any others.
  Without diagnostics the driver drops both (LEGAL-63).
  The window, its record and its address on each target are [the contract with sg](../../reserved-bindings.md).
  A compile declares the window only where a statement reaches it.
  A stage the contract says cannot reach it drops its prints and declares none.
  That is WebGPU's vertex stage, and vulkan's vertex, geometry and tessellation stages without `vertex_pipeline_stores`, which no program names.
  On metal a ray-tracing pipeline's raygen takes the window and hands it on in `sgl_context`, since every other stage of it is a function the raygen calls.
  It takes it whether it prints or not, and each stage that prints reads it from there.
  A site's id hashes the entry point's name, the message and the line it stands on, so one site has one id on every target.


The rules above say HLSL and WGSL by name; these say what `msl` writes in the same places.

* **EMIT-56** After the comment of EMIT-47, the text is `#include <metal_stdlib>`, `using namespace metal;` and an empty line.
* **EMIT-57** MSL's reserved words also hold every name the Metal toolchain declares at global scope or in `metal`, its macros included, and `main` ([why](why/emitting.md#emit-57)).
  They hold the intersection tags `instancing` and `triangle_data` too, which a ray-tracing stage's `using namespace raytracing;` brings in.
* **EMIT-58** An `@inline binding` is a struct of its members and the parameter `constant T& name [[buffer(4)]]` of the entry point ([why](why/emitting.md#emit-58)).
* **EMIT-59** The entry point is a `vertex`, `fragment` or `kernel` function, and its SGL parameter carries `[[stage_in]]`.
  MSL has no spelling for a kernel's workgroup, so the text states none, and the shape reaches sg from what SGL states alone.
* **EMIT-60** A member with `@position` is `[[position]]`.
* **EMIT-61** A member at location i is `[[attribute(i)]]` in a vertex input, `[[user(sgli)]]` in a stage link, and `[[color(i)]]` in a render target struct.
* **EMIT-62** MSL's own rule places `float3` at a multiple of 16 and gives it 16 bytes, so a block's `float3` is `packed_float3` in its memory form (EMIT-113) ([why](why/emitting.md#emit-62)).
* **EMIT-63** The product is `m * v`, an immutable local is `const T name = value;`, and a struct is built as EMIT-55 builds it ([why](why/emitting.md#emit-63)).
* **EMIT-64** A float literal has no suffix in MSL either ([why](why/emitting.md#emit-64)).

```cpp
vertex pixel_input main_vs(cube_vertex v [[stage_in]], constant constants_data& constants [[buffer(4)]])
{
    pixel_input result;
    result.position = constants.view_projection * float4(v.position, 1.0);
    result.normal = v.normal;
    result.color = v.color;
    return result;
}
```

So `{float3; float}` is written with `packed_float3`: the `float` is at byte 12 on every target, as the layout rules say.

## Ray tracing

[raytracing](../raytracing.md) is the model; these say what each target writes of it.

* **EMIT-138** A member whose name is the name of a struct type one of its struct's members has is minted with trailing underscores, on every target ([why](why/emitting.md#emit-138)).
  In HLSL and MSL the member would hide the type from the members after it, so `ray: ray` is written `ray ray_;`.
* **EMIT-135** A trace's native form is what HLSL and MSL write, and its emulated form what WGSL writes: the legalizer keeps the one its target has ([why](why/emitting.md#emit-135)).
  A compile with `software_trace` keeps the emulated form on HLSL and MSL too, an MSL entry point taking the pool and the roots at their argument-table slots.
  An MSL pipeline trace stays native, with the motion tags (EMIT-139) and its ray's time, since Metal's intersector places what moves and its query does not.
  An acceleration member is `RaytracingAccelerationStructure` in HLSL and `instance_acceleration_structure` in MSL, and WGSL declares none: it keeps its slot and takes no binding.
  An entry point whose code reads sg's acceleration pool declares it and the dispatch's roots in sg's reserved group, beside the inline constants:
  `@group(3) @binding(17) var<storage, read> sg_acceleration_pool: array<vec4u>;` and `@group(3) @binding(18) var<uniform> sg_acceleration_roots: array<vec4u, 4>;`.
  The root of the entry point's k-th acceleration member (CHK-325) is `sg_acceleration_roots[k / 4][k % 4]`.
  So WGSL traces the first 16 acceleration members, and a trace of a later one is `too-many-acceleration-structures`.
  HLSL's are the same pool and roots, as a `StructuredBuffer<uint4>` and a `cbuffer` holding `sg_acceleration_roots`.
  dx12 places them at `register(t0, space12)` and `register(b0, space12)`, and vulkan at `[[vk::binding(17, 3)]]` and `[[vk::binding(18, 3)]]`.
  The member stays declared there, and takes the same 16-member limit.
* **EMIT-159** HLSL's `RayQuery` takes a trace's flags as its template where they are a constant, each by its `RAY_FLAG_` name, and `RAY_FLAG_NONE` where they are not.
  A local given a constant and never assigned again is that constant, which is how a mixed world's trace hands its flags on.
  The template is ORed with the trace's own flags, so naming them there changes nothing but what a driver may specialize on.
  Forced opaque, the native form only steps the query to its end, since the target commits every candidate itself and none is left to decide.
* **EMIT-184** An HLSL compile for a device with `opacity_micromap` writes every `RayQuery` with `RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS`, so its traces see the micromaps their BLASes hold.
  The entry point then states the feature, which no program names, and is compiled at Shader Model 6.9.
  vulkan sees micromaps without the flag, so it writes the flag only on a query whose flags force 2 states or are no constant, since DXC takes a forced ray only beside it.
  An entry point with such a query declares `SPV_EXT_opacity_micromap`'s capability, which DXC leaves out.
  Either one on a query that needs neither makes DXC take minutes over a large kernel and can make it write invalid SPIR-V.
  `ray_flags.force_omm_2_state` is in a `RayQuery`'s template only there, since only 6.9 names it; elsewhere the template leaves it out, and the trace's flags still carry it.
  A native route states the same: `SPV_EXT_opacity_micromap`'s capability on SPIR-V, whose traces see micromaps with no flag of their own.
  On DXIL `dx.op.allocateRayQuery2` with the flag that allows them, at Shader Model 6.9, a library's function row included.
* **EMIT-260** A hit's `cluster_id` is read only where the device builds a BLAS from clusters, `clustered_geometry`, and is -1 everywhere else.
  The prelude asks with `has`, so no program names the feature, and an entry point on any other device needs nothing of it.
  SGL's SPIR-V writes `OpRayQueryGetIntersectionClusterIdNV` and `ClusterIDNV`, with `SPV_NV_cluster_acceleration_structure`'s capability.
  HLSL for vulkan reaches them through inline SPIR-V, a template over both of a `RayQuery`'s parameters and a builtin input, which DXC has no names for.
  HLSL for dx12 writes Shader Model 6.10's `CandidateClusterID`, `CommittedClusterID` and `ClusterID`, and DXIL refuses it as `unsupported-yet` until then.
* **EMIT-185** A text keeps its entry point's float guarantee (CHK-435), or the compile refuses with `float-guarantee-not-kept`, naming the function that asked.
  `hlsl-dx12` keeps every level: DXC compiles from `.specials` on with `-Gis`, and at `.ieee` with `-denorm preserve` too.
  `msl` keeps up to `.precise`, since Metal's safe math keeps the special values and the order and flushes denormals, read and written (measured on an M4).
  `hlsl-vulkan` keeps every level through the same flags: DXC writes `SignedZeroInfNanPreserve` and `NoContraction` on every float operation for `-Gis`, and `DenormPreserve` for `-denorm preserve`.
  Each mode needs its device property, so a vulkan entry point needs the feature `float_specials` from `.specials` on, and `float_denormals` at `.ieee`.
  `wgsl` keeps `.fast` alone.
  A host may weaken a guarantee on WGSL alone, which compiles at `.fast` with the warning `float-guarantee-weakened`.
* **EMIT-186** HLSL writes `!=` of float or half lanes as `!(a == b)`, and a whole vector's as `any(!(a == b))`, which is true where either is a NaN.
  DXC lowers HLSL's own float `!=` to SPIR-V as `OpFOrdNotEqual`, false for a NaN, where DXIL's is unordered; the negated `==` is unordered on both.
  WGSL and MSL write `!=`, which is unordered in both.
* **EMIT-210** HLSL writes an unbounded binding array (CHK-490) as `name[]`, at the register or vulkan binding its slot gives, and the pipeline layout leaves its range open.
* **EMIT-211** An entry point that needs `unbounded_binding_arrays` is `target-lacks-feature` in WGSL, which has no binding arrays.
  MSL takes no unsized member in an argument buffer, so an unbounded array's slot is a `constant <name>_table*` to a table its group sizes.
  The table is its element count, then `entries[1]` its elements are read through, then each element's byte length, which a length reads (EMIT-195).
* **EMIT-195** HLSL reads a buffer's length (CHK-460) through `GetDimensions`, in a helper per buffer, and WGSL through `arrayLength`, four bytes per word of a `bytes`.
  A Metal buffer is a pointer, so each argument buffer ends in `uint sgl_lengths[n]`, one per slot, which sg fills with the byte length of each slot's buffer view.
  MSL reads a buffer's length as its slot's entry, plus the element of a binding array, divided by the element's size, and a `bytes`' as the entry itself.
  An element of a binding array takes a helper with the index as its parameter, and one per mark, since `NonUniformResourceIndex` must stand inside it.
* **EMIT-196** Neither MSL nor DXIL carries a 64-bit value between stages, so on `hlsl-dx12` and MSL a link's 64-bit scalar crosses as a `uint2` of its words.
  A two-vector crosses as a `uint4`, each low word first, and either keeps the member's one location.
  A three- or four-vector crosses as two `uint4`s at its two locations, the second holding its last one or two lanes; a three-vector's last two words are zero.
  The member is declared as its piece `<name>_0`, and an array of them as one per element, as EMIT-174 declares a link's array.
  A write splits the value, through `asuint` for a double in HLSL and `as_type` in MSL, and a read joins the words back, so the program never sees them.
  SPIR-V carries the value itself, so `hlsl-vulkan` writes it as declared, a three- or four-vector at the first of its two locations.
* **EMIT-218** MSL writes a buffer's `atomic[ulong]` as `atomic_ulong`, and its `min` and `max` as `atomic_min_explicit` and `atomic_max_explicit`, which give nothing back.
  So each stands as a statement alone, and an MSL compile whose program reads what one gave back is `target-lacks-feature`.
  HLSL writes a workgroup memory's 64-bit atomic as a `groupshared` `int64_t` or `uint64_t` under the same `Interlocked*` calls as a buffer's.
* **EMIT-219** On dx12 a double's maths (CHK-458) is the SGL of `core.sgl`, over a double's bits (CHK-517), its arithmetic and `fma` ([why](why/emitting.md#emit-219)).
  DXIL has no double `sqrt`, rounding or root, and DXC hands a double to its own `sqrt` or `floor` as a float, so neither dx12 route calls a target's.
  `sqrt` and `inverse_sqrt` start from the float `inverse_sqrt` of the mantissa brought to 1 to 4, take two Newton steps, and `sqrt` corrects by the residual `fma` gives.
  A rounding clears the fraction's bits, so `trunc`, `floor`, `ceil`, `round` and `fract` are exact there.
  `sign` and `saturate` are HLSL's own, which take a double.
* **EMIT-256** A double's maths is one SGL body per function in `core.sgl`, which chooses by `target_api` (CHK-426) what it lowers to ([why](why/emitting.md#emit-256)).
  dx12 takes EMIT-219's bodies, on both its routes.
  vulkan takes GLSL.std.450's own on a double, on both its routes: `Trunc`, `Floor`, `Ceil`, `RoundEven`, `Fract`, `Sqrt` and `InverseSqrt`.
  The native SPIR-V route writes them as instructions, and `hlsl-vulkan` as functions of DXC's inline SPIR-V the text declares, `sgl_spirv_floor` and its siblings, since DXC's own take a float.
  So the two routes of one API compute alike, and the two APIs need not.
  The interpreter runs EMIT-219's bodies, except that `sqrt`, `inverse_sqrt` and `smoothstep` take its own answer where the language is `.sgl` (EVAL-108).
* **EMIT-255** HLSL writes a double's bits (CHK-517) as functions of the text, `sgl_double_bits` over `asuint` and `sgl_double_from_bits` over `asdouble`.
  `asuint` hands a double's words to two out parameters, which no expression can take.
  A native route bitcasts: SPIR-V's `OpBitcast` between a double and a `uint2`, and DXIL's `splitDouble` and `makeDouble` (EMIT-196).
* **EMIT-258** SPIR-V's atomics of workgroup memory state `Workgroup` scope, and every other `Device`.
  A 64-bit atomic store of workgroup memory is an `OpAtomicExchange`, and a load an `OpAtomicOr` of 0, as DXC writes them.
  Intel's vulkan driver reads wrong after either plain form: [bug-sgl-intel-vulkan-shared-64-bit-atomic-load-store](../../issues/bug-sgl-intel-vulkan-shared-64-bit-atomic-load-store.md).
* **EMIT-259** Every target writes an `affine3` in GPU memory as CHK-518's three rows.
  HLSL declares it `row_major float3x4`, and a buffer of them holds each in a struct of one field: DXC's DXIL drops a `row_major` written in a buffer's template.
  WGSL's `mat3x4f` and MSL's `float3x4` hold the rows as their own columns.
  A native route holds it as a matrix of three `float4` columns, one per row, at a stride of 16, which SPIR-V decorates `ColMajor`, as DXC does for HLSL's `row_major float3x4`.
* **EMIT-254** `fma` is each target's own: HLSL's `fma` of a double and `mad` of a float or a half, and WGSL's and MSL's `fma`.
  A native route writes DXIL's `dx.op.fma` of a double and `dx.op.fmad` of the others, and SPIR-V's GLSL.std.450 `Fma`, as DXC writes them.
  Only a double's on DXIL is stated to round once, as HLSL's documentation of `fma` says, and every other may round the product first.
  `mad` and `dx.op.fmad` are free to fuse or not, and Vulkan's precision table states `Fma` as a multiply followed by an add, so a vulkan double may round twice too.
  WGSL states `fma` as `e1 * e2 + e3`, and nothing here relies on MSL's rounding once.
* **EMIT-225** `hlsl-vulkan` writes a hit object (CHK-434) and every reordering (CHK-433) as `SPV_EXT_shader_invocation_reorder`'s instructions, since DXC lowers none of HLSL's to SPIR-V.
  Each instruction is a function of DXC's inline SPIR-V the text declares ahead of the entry point, and a hit object is `vk::SpirvOpaqueType<5313>`, SPIR-V's `OpTypeHitObjectEXT`.
  An instruction that hands a payload over takes a variable of the ray-payload storage class, one per payload type, which the call fills before and reads back after.
  A reordering trace is one `OpHitObjectTraceReorderExecuteEXT`, and `maybe_reorder` is `OpReorderThreadWithHintEXT`.
  A hit object's `let` declares nothing: SPIR-V forbids loading or storing a hit object, so the `let` names the variable its trace filled.
  That variable is a `Function` pointer to the type, which is how the instructions take a hit object, and all a module may do with it.
  A parameter holding one is the same name, since a call is inlined with its arguments standing in, so a hit object handed to a function is no copy.
  What would be a copy, CHK-516 refuses before any text: DXC would lower it to `OpLoad` and `OpStore`, which `spirv-val` and the validation layer accept and a driver may crash on.
  `sgl hit object spirv` in slib's tests reads DXC's module word by word to hold this, every path at `-O3` and `-Od`, and every raygen of the repo that reorders.
* **EMIT-229** A compute stage whose workgroup memory LEGAL-69 shadows declares the shadow `sgl_written`, a `uint` atomic word per 32 scalars, beside that memory.
  A word of the shadow is `sgl_written[w]`, and the prologue that clears it, or zeroes workgroup memory under `define`, is statements LEGAL-71 wrote.
  WGSL has none, since WebGPU's workgroup memory starts at zero, the shadow's included.
  An invocation's index is its `@local_thread_index` where the stage takes one, and a parameter legalize adds where it does not.
* **EMIT-136** HLSL writes a ray-tracing stage as a library export named by its kind, `[shader("closesthit")]` over `void`, whose payload is an `inout` parameter.
  A triangle closest hit or any hit takes `BuiltInTriangleIntersectionAttributes` too.
  An any hit's decision ends the stage: `ignore` is `IgnoreHit()`, `accept_and_end_search` `AcceptHitAndEndSearch()`, and `accept` a plain `return`.
  A trace of a ray type is `TraceRay` over a `RayDesc`, and a callable's call is `CallShader`.
* **EMIT-137** A payload is a `[raypayload]` struct, and each field states which stages read and write it, `read(caller, closesthit) : write(miss)` ([why](why/emitting.md#emit-137)).
  They are inferred from every entry point of the module: a stage reads and writes what its payload parameter does.
  A caller reads and writes what every local of the payload's type does, in an entry point that traces the type: wider than the one local it traces with, and never narrower.
  A payload a stage hands on to a trace or a callable is read and written by that stage, since what the nested shaders write must survive its exit.
  A stage that writes a field reads it too, since a write on some paths keeps the rest, and whatever some stage writes the caller reads, and the reverse.
  A payload type that a pipeline with `.host` groups traces, or that no pipeline of the module traces, states the widest access instead.
  SGL's own DXIL states the same access in `!dx.dxrPayloadAnnotations`, field for field as DXC writes it for the text.
  An intersection's report is `ReportHit(t, 0, attributes)` where it hits, and a procedural record's shaders take the attributes as a parameter of their own.
* **EMIT-139** MSL writes a pipeline as a kernel that intersects and then calls through sg's tables ([why](why/emitting.md#emit-139)).
  Every function of the pipeline agrees on the shapes below, since each is compiled apart.
  * The raygen is a `kernel`, which takes the argument buffers, sg's tables at `[[buffer(3)]]` and each instance's hit-group offset at `[[buffer(5)]]`.
  * A miss, a closest hit and a callable are `[[visible]]` functions of one signature: the payload as `uint4` words, a hit record but for a callable, and an `sgl_context`.
    The context carries the argument buffers, the inline constants, the tables and the launch, and the function binds each to a local.
  * An any hit is an `[[intersection(triangle, triangle_data, instancing, world_space_data)]]` function.
    It answers whether it accepts and whether the search goes on, and writes the payload back into the ray data.
    `accept_and_end_search` accepts and ends the search through `[[continue_search]]`.
  * A procedural record is its traversal entry point (CHK-345), an `[[intersection(bounding_box, triangle_data, instancing, world_space_data)]]` function.
    Where its fused any hit decides `accept_and_end_search`, its result ends the search the same way.
  * A trace is the kernel's own `intersector`, configured from the ray flags, then the miss or the closest hit through the tables.
    The closest hit's record is the instance's offset plus the geometry index times the multiplier plus the contribution, as DXR finds it.
  * The ray data holds as many words as the largest payload of the set, so every shader of a pipeline agrees on it.
    The set is the one of the pipeline or the hit group holding the shader, and one it traces only where none holds it.
    A shader that traces or stands in traversal, held by two whose sets' payloads differ, is `ray-data-conflict`, since one text cannot agree with both.
  * Every table, intersection function and intersector carries `world_space_data`, the tag under which Metal hands over the instance's transforms.
    A closest hit's object ray is the world ray through the world-to-object transform.
  * Every one closes its tags with `sgl_motion`, which a file that traces defines once after `<metal_raytracing>`.
    A compile with `software_trace` defines it as `, instance_motion, primitive_motion`, and every other compile as nothing.
    `sgl_motion_time(t)` beside it passes a trace's time to `intersect` under the tags, and nothing where there are none.

## Subgroups, coherence and image atomics

* **EMIT-146** A subgroup operation is the target's own, by the table below, with its lane converted to the target's lane type.
  HLSL writes an inclusive prefix as the exclusive one combined with the invocation's own value, and a shuffle by an offset as `WaveReadLaneAt` of the lane it computes.
  Its bitwise reductions take unsigned integers alone, so one of `int`s reduces them as `uint`s and converts back.
  MSL's ballot is 64 bits, which fill the low two components of the `uint4`.
* **EMIT-147** `@subgroup_size` and `@subgroup_invocation_id` are `WaveGetLaneCount()` and `WaveGetLaneIndex()` in HLSL, read into locals at the top of the entry point.
  WGSL takes them as `@builtin(subgroup_size)` and `@builtin(subgroup_invocation_id)`, and MSL as `[[threads_per_simdgroup]]` and `[[thread_index_in_simdgroup]]`.
* **EMIT-148** `hlsl-dx12` writes `@preferred_subgroup_size(n)` as `[WaveSize(4, 128, n)]`, the range form with `n` preferred, which every dx12 device runs ([why](why/emitting.md#emit-148)).
  No other text states it: the preference reaches the host beside the text, as a kernel's shape does in MSL (EMIT-59).
  sg's vulkan backend asks the pipeline for subgroups of `n` where the device runs that size in a compute stage, and WGSL and MSL have no way to ask.
* **EMIT-149** `workgroup_uniform_load(m)` is `workgroupUniformLoad(&m)` in WGSL.
  HLSL and MSL write the workgroup barrier of EMIT-131, read `m` into a local, and write the barrier again ([why](why/emitting.md#emit-149)).
* **EMIT-150** A `@coherent` member is `globallycoherent` in HLSL, which DXC writes as SPIR-V's `Coherent` for vulkan.
  In MSL, from 3.2, a buffer's pointer carries `coherent(device)`, and a texture takes `memory_coherence_device` as its last template argument.
  An argument buffer's field takes no `coherent` qualifier, which is why the two differ.
  The barrier that publishes its writes is EMIT-131's, which is device-scoped in HLSL; WGSL lacks `device_coherence` (EMIT-109).
  MSL's `threadgroup_barrier` orders memory within the threadgroup alone, so an entry point holding a `@coherent` member follows a `storage_barrier` or a `texture_barrier` with a device-scoped fence.
  That fence is `atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device)`, or `mem_texture` for the texture barrier.
* **EMIT-151** An image subscript is the `load` or the `store` it stands for (CHK-367), and a compound assignment through one evaluates its coordinates once and loads the texel once.
  A thread sees its own store to a `read_write` texture in MSL only after `img.fence()`, where HLSL and WGSL order one invocation's own accesses.
  So MSL writes `img.fence();` behind every store to an image the entry point also reads.
  An `@atomic` image is the plain integer image in HLSL, and its texel's update is `InterlockedMax(img[xy], v, before)`, read back as a buffer atomic's is (EMIT-120).
  MSL makes it a `read_write` texture and calls its own methods, `img.atomic_fetch_max(xy, v)`, `atomic_load` and `atomic_store`, from MSL 3.1.
  WGSL lacks `image_atomics` (EMIT-109).
* **EMIT-152** An option is written as the value the compile gave it, a literal, as every `const` is: no text names an option, and none uses its target's specialization constants.
  A branch CHK-356 removes is in no text.

| SGL | HLSL | WGSL | MSL |
|---|---|---|---|
| `subgroup_all`, `subgroup_any` | `WaveActiveAllTrue`, `WaveActiveAnyTrue` | `subgroupAll`, `subgroupAny` | `simd_all`, `simd_any` |
| `subgroup_ballot` | `WaveActiveBallot` | `subgroupBallot` | `simd_ballot` |
| `subgroup_add`, `subgroup_mul` | `WaveActiveSum`, `WaveActiveProduct` | `subgroupAdd`, `subgroupMul` | `simd_sum`, `simd_product` |
| `subgroup_min`, `subgroup_max` | `WaveActiveMin`, `WaveActiveMax` | `subgroupMin`, `subgroupMax` | `simd_min`, `simd_max` |
| `subgroup_bit_and`, `subgroup_bit_or`, `subgroup_bit_xor` | `WaveActiveBitAnd`, `WaveActiveBitOr`, `WaveActiveBitXor` | `subgroupAnd`, `subgroupOr`, `subgroupXor` | `simd_and`, `simd_or`, `simd_xor` |
| `subgroup_exclusive_add`, `subgroup_exclusive_mul` | `WavePrefixSum`, `WavePrefixProduct` | `subgroupExclusiveAdd`, `subgroupExclusiveMul` | `simd_prefix_exclusive_sum`, `simd_prefix_exclusive_product` |
| `subgroup_inclusive_add`, `subgroup_inclusive_mul` | the exclusive one, then `+ x` or `* x` | `subgroupInclusiveAdd`, `subgroupInclusiveMul` | `simd_prefix_inclusive_sum`, `simd_prefix_inclusive_product` |
| `subgroup_broadcast`, `subgroup_broadcast_first` | `WaveReadLaneAt`, `WaveReadLaneFirst` | `subgroupBroadcast`, `subgroupBroadcastFirst` | `simd_broadcast`, `simd_broadcast_first` |
| `subgroup_shuffle` | `WaveReadLaneAt` | `subgroupShuffle` | `simd_shuffle` |
| `subgroup_shuffle_xor`, `_up`, `_down` | `WaveReadLaneAt` of the lane `WaveGetLaneIndex()` gives | `subgroupShuffleXor`, `subgroupShuffleUp`, `subgroupShuffleDown` | `simd_shuffle_xor`, `simd_shuffle_up`, `simd_shuffle_down` |
| `quad_swap_x`, `quad_swap_y`, `quad_swap_diagonal` | `QuadReadAcrossX`, `QuadReadAcrossY`, `QuadReadAcrossDiagonal` | `quadSwapX`, `quadSwapY`, `quadSwapDiagonal` | `quad_shuffle_xor` by 1, 2 and 3 |
| `quad_broadcast` | `QuadReadLaneAt` | `quadBroadcast` | `quad_broadcast` |

## Layout

Every value in GPU memory — a constant block or a buffer's element — is placed by one rule, the same on every target.
**The rule is the one that loads fastest**: every vector aligned to its size, so every target reads it whole ([why](why/emitting.md#emit-110)).
Where a host struct needs what C++ would place instead, `@padding(.none)` asks for it (CHK-369), and nothing else does.
The C++ struct a package generates is that layout byte for byte, padding included, so the host copies it in as it is.
**That struct is the only thing the host may rely on**: without an annotation, where a member lands is the compiler's choice (EMIT-116).

* **EMIT-110** A value in GPU memory stands at its **natural alignment**: a scalar at its size, a 2-vector at twice that, a 3- or a 4-vector at four times that, and a matrix at its column's.
  So a `float3` starts at a multiple of 16 and takes 12 bytes, and a scalar that follows it fills its tail.
  A 16-bit value is aligned likewise, at 2, and a `half3` at 8.
  A struct aligns to its most aligned member, and its size is where its last member ends, rounded up to that alignment.
  In a constant block a nested struct also starts a 16-byte row, and what follows it starts the next one.
  A constant block's own size is where its last member ends.
  No vector crosses a 16-byte row, so a constant buffer read by rows reads each one from one.
* **EMIT-111** A buffer's element is placed by EMIT-110, and the buffer strides by the element's size.
  A builtin element's size is rounded up to its alignment too, so `buffer[float3]` strides by 16, and CHK-381 holds every stride to whole 4-byte words.
  The host writes such an element as `slib::gpu_padded<tg::vec3f, 16>`.
* **EMIT-112** Each target is made to follow the rule ([why](why/emitting.md#emit-112)).
  WGSL's storage rule is this one, and its uniform rule too, so WGSL writes every root as it is.
  MSL's is this one but for a 3-vector, which takes 16 bytes there: a 3-vector whose tail holds a value is its `packed_` type in the root's memory form (EMIT-62).
  HLSL packs a structured buffer as C++ does and a constant buffer within its rows, so it writes a padding field into each gap the rule leaves, a struct's tail included.
  A struct written so is declared with its padding wherever it stands, and a local of it is zeroed before its members are assigned, since DXIL refuses to store an undefined value.
  A buffer of 3-vectors is a one-field memory form in HLSL, its padding a second field.
  `hlsl-vulkan` also states every offset, which sg's vulkan backend admits by requiring `scalarBlockLayout`.
* **EMIT-113** A memory form is one struct of the root's builtin values in memory order, with padding fields where the layout leaves room.
  A value the target places at its offset natively is a field of its own type.
  A vector that it does not is split into scalar fields in WGSL, and is its `packed_` type in MSL; a matrix is split into its scalars.
  A read of a value rebuilds it from its fields, and a write stores each field; a whole struct is stored once into a local, then field by field.
* **EMIT-114** A struct has one layout, so every binding places it by one rule: in a constant block, in a buffer's element, packed in either, never two of them.
  A struct placed by two is `layout-conflict`.
  A struct with a target layout (CHK-522) places its fields in another order per compile, and each compile places that order by this one rule.
* **EMIT-115** `@no_padding` on a struct or on a binding makes a gap before any of its members `padding-forbidden`, and the detail says where.
  The rest of a block's last row follows no member, so it is no gap.
* **EMIT-116** A layout carries no guarantee without an annotation that asks for one ([why](why/emitting.md#emit-116)).
  The compiler may place members in another order than they are declared, to pack them tighter.
  Host code reaches GPU memory through the generated struct, never through offsets or an order it assumed.
* **EMIT-153** *Retired:* `@layout(.hlsl)` promised HLSL's constant-buffer packing, which no rule places by any more.
* **EMIT-154** A binding marked `@padding(.none)` has its constant block placed as a C++ compiler places a struct of the generated host types ([why](why/emitting.md#emit-154)).
  Each value stands at the next multiple of its scalar's size, in declaration order, and that layout is promised: the compiler never reorders it.
  A struct's size is rounded up to its alignment, a `float3` takes 12 bytes, and no row rule applies.
  Every target writes the block as its memory form (EMIT-113), and in every target a vector the layout lets cross a 16-byte row is split into scalars.
  `hlsl-vulkan` states every field's offset.
  `hlsl-dx12` states none: each field fits its row and padding fills each gap, so its own packing lands every field there ([why](why/emitting.md#emit-154)).
  A struct member of such a block is `unsupported` for now, and so is an array C++ places anywhere but at a multiple of 16 bytes.
* **EMIT-164** An array in a constant block, whose element takes whole 16-byte rows (CHK-291), starts a row and holds its elements back to back.
  Every target places it so natively, so a memory form holds it as one field of the array type, and a read takes it in place: a runtime index needs no copy.
  The host holds it as a `cc::fixed_array` of the element's host type.
* **EMIT-165** A computed `const` array (CHK-219) is bound once per tree to a local of its name, at the start of the tree's body, and every read of it indexes that local.
  A target writes a constructed array whole wherever it stands, so read in place it would be written once per index: S2H's 192-word font table reached 1.5 MB of text.
  A vector, a matrix or a struct const is written where it is read, since it is no larger than the expression that reads it.
* **EMIT-169** HLSL holds an array of 3-vectors in GPU memory as one of 4-vectors, which a structured buffer strides by 16 as a constant buffer does ([why](why/emitting.md#emit-169)).
  An element reads as its first three lanes, `a[i].xyz`, and is written through them.
  A read of the whole array rebuilds it into a local element by element, and a write of it stores each element so.
  A struct that stands in memory declares the 4-vectors as its field, so this holds wherever the struct is.
* **EMIT-170** MSL declares a 3-vector of a struct in GPU memory as its `packed_` type where the next member sits in its tail ([why](why/emitting.md#emit-170)).
  A padding field keeps it at its offset, since a packed vector aligns to its scalar alone, and `alignas` keeps the struct's own alignment.
  A read converts it back, `float3(s.color)`, and a write converts into it implicitly.
  So the struct is placed as SGL places it wherever it stands, in an array too, and a buffer of it needs no memory form.
  A constant block's own members are declared the same way, so MSL writes a memory form only for `@padding(.none)`.
* **EMIT-172** A constant block's array marked `@padding(.every_element)` takes a 16-byte row per element ([why](why/emitting.md#emit-172)).
  Every target declares it as an array of 4-vectors of the element's scalar.
  An element reads as its own lanes, `a[i].x` or `a[i].xy`, and a read of the whole array rebuilds it into a local element by element.
  The host writes each element as `slib::gpu_padded<T, 16>`.
* **EMIT-173** How each expression that reaches GPU memory is spelled is decided once per entry point and target, before any text is written ([why](why/emitting.md#emit-173)).
  What the target holds is the value itself, a widened `mat3` (EMIT-168), a widened array (EMIT-169, EMIT-172), or a packed tail (EMIT-170).
  An assignment's place, the objects a write through it reaches, and an array indexed in place are spelled as held; every other read converts back.
* **EMIT-174** A vertex input and a stage link declare a matrix member as a member per column, and an array member as a member per element ([why](why/emitting.md#emit-174)).
  A vertex input's piece `i` has its member's semantic at index `i`, `FRAME0`, and the host's layout names it the same way: one attribute per piece, at the member's offset plus `i` pieces.
  The pieces are named `<member>_<i>` at consecutive locations, every member after them moves on by as many, and the member's interpolation is every piece's.
  A read rebuilds the value: a matrix from its columns, an array into a local element by element, and an element at a literal index reads its piece alone.
  A write stores each piece of the value, evaluated once, and a write into one at a runtime index or through a column gathers it into a local, writes that, and stores it back.
  A struct that both crosses a stage edge this way and stands in GPU memory is `unsupported`, since the pieces are no layout that memory has.
* **EMIT-175** A `mat2` in GPU memory is its two columns back to back: 16 bytes, aligned to 8, and to a row in a constant block ([why](why/emitting.md#emit-175)).
  HLSL's constant buffer starts each column of a matrix on a row, so a constant block, and a struct that stands in one, holds a `mat2` there as one `float4`.
  A read rebuilds it through `sgl_narrow_mat2`, which the text declares wherever it reads one; a constant block is never written.
  Every other memory holds it as each target's own `mat2`, which MSL keeps on its row with a padding field.
  Under `@padding(.none)` it is 16 bytes aligned to 4, as `tg::mat2f` is: HLSL's own `float2x2` in a structured buffer, and split where another target's would not sit (EMIT-178).
  A packed constant block's `mat2` is `unsupported`, alike on every target, since HLSL's constant buffer would start each column on a row.
* **EMIT-176** MSL places a member of a struct or a block in natural memory where SGL placed it, with padding fields wherever MSL would place it sooner.
  A struct that stands in a constant block is declared `alignas(16)`, so it starts a row and an array of it strides by rows, as EMIT-110 has it.
* **EMIT-177** MSL states every layout SGL placed as `static_assert`s the Metal compiler checks ([why](why/emitting.md#emit-177)).
  One per member of a struct, a block and a memory form in GPU memory, `__builtin_offsetof(T, m) == offset`, and one per struct, `sizeof(T) == size`.
  A struct of a constant block is asserted at its size rounded up to a row, and a block at its members alone.
* **EMIT-178** A buffer element's array of builtins under `@padding(.none)` strides by its element's scalars, as C++ places it ([why](why/emitting.md#emit-178)).
  HLSL's structured buffer places it so already; WGSL and MSL declare it in the element's memory form as one field.
  That field is the target's own array where its stride and alignment match, MSL's array of `packed_` vectors where they do not, and otherwise one array of every element's scalars, back to back.
  An element of a split array reads its scalars at `k * i + c`, its index evaluated once, and a whole array reads and writes element by element.
  A packed matrix is split column by column, a `mat2` too.
* **EMIT-171** A packed array of structs or of arrays waits, `unsupported` alike on every target, as does a packed block's array other than EMIT-154's.
  A packed block's array of whole rows is the exception, which EMIT-154 places.
  Any other multi-dimensional array nests the natural stride: each inner array strides by its size rounded up to its alignment, and every target declares it as nested arrays.
  HLSL widens a 3-vector at any depth, `float4 dirs[2][3]`, and only the innermost element reads through its lanes.
  A `mat3` two or more dimensions deep stands in a `sgl_mat3_cell` of HLSL, one `column_major float4x3 m`, of the same 48 bytes, and is read through `.m` ([why](why/emitting.md#emit-171)).
* **EMIT-168** HLSL holds a `mat3` in GPU memory as a `column_major float4x3`, three whole rows of a constant buffer and 48 bytes of a buffer ([why](why/emitting.md#emit-168)).
  A read narrows it, `((float3x3)m)`, and a write widens a value through `sgl_widen_mat3`, which the text declares wherever it holds one.
  A struct that stands in memory declares the `float4x3` as its field, so a read of the field narrows wherever the struct is.
  An array of `mat3` is one of `float4x3`: an element narrows and widens as one `mat3` does, and a whole read or write goes element by element.
  Under `@padding(.none)` a `mat3` is HLSL's own `float3x3`, 36 bytes in a buffer; WGSL and MSL split that one into its scalars in a memory form.
* **EMIT-167** A buffer whose binding or member is marked `@padding(.none)` has its element placed as a C++ compiler places it, in declaration order, and promised.
  Each value stands at the next multiple of its scalar's size, a struct's size is rounded up to its largest scalar, and `buffer[float3]` strides by 12.
  HLSL's structured buffer packs so natively; WGSL and MSL write the element as its memory form wherever their own rule would not.

## Error kinds

| kind | reported by |
|---|---|
| `module-has-errors` | EMIT-10 |
| `unknown-entry-point` | EMIT-11 |
| `unsupported` | EMIT-12, EMIT-33, EMIT-34, EMIT-38, EMIT-39, EMIT-67, EMIT-81, EMIT-107, EMIT-154 |
| `reserved-entry-point-name` | none: retired by EMIT-21 |
| `system-value-semantic` | EMIT-32 |
| `layout-mismatch` | none: retired by EMIT-41 |
| `layout-conflict` | EMIT-114 |
| `padding-forbidden` | EMIT-115 |
| `non-finite-literal` | EMIT-50 |
| `malformed-tree` | a flat tree the check pass does not produce |
| `not-core` | EMIT-66 |
| `too-many-groups` | EMIT-105 |
| `too-many-samplers` | EMIT-133 |
| `too-many-acceleration-structures` | EMIT-135 |
| `ray-data-conflict` | EMIT-139 |
| `target-lacks-feature` | EMIT-109, EMIT-122, EMIT-206, EMIT-261 |

## Open

* GLSL, which comes through the same seam.
* The arrays EMIT-171 refuses: packed ones of structs or of arrays, and multi-dimensional ones of `mat3`.
* `@padding(.every_column)`, which would name a `mat3`'s natural 48 bytes explicitly; `.none` gives the 36 of `tg::mat3f` already.
* A packed constant block's `mat2`, which HLSL would split into its scalars across rows.
* A struct member of a `@padding(.none)` block, which HLSL would read back by building the struct in a local field by field.
* `@padding` on a struct, which would place it so wherever it stands.
* Whether an emit error becomes a diagnostic with a span; today it names a symbol and carries a detail.
* Whether the size of an inline block has to agree between targets as its offsets do; WGSL rounds it up to 16 bytes.
* How a vertex input's dx12 semantic is chosen once a member wants one that is not its name.
* How a splatted value reads once a target can take the vector whole ([checking](checking.md#open)).
* Whether the reserved words of a target hold every function of that target, or only the ones a builtin is written as.
