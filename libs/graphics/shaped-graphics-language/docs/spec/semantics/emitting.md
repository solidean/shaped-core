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
* **EMIT-4** No target depends on a flag of the compiler that reads its text ([why](why/emitting.md#emit-4)).
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
* **EMIT-13** No error depends on the target but EMIT-109's and EMIT-122's: an entry point is written for every target or for none ([why](why/emitting.md#emit-13)).
* **EMIT-109** An entry point that needs a feature no device of the target has is `target-lacks-feature`, and its detail names the feature.
  Today that is `wgsl` against `binding_arrays`, `multisampled_array_textures`, `raytracing_pipeline`, `geometry_shader` and `tessellation_shader`.
  `ray_query` is none of them: WGSL writes a trace's emulated form (EMIT-135).
  The shader chose it by `require`, so a portable shader still meets EMIT-13's promise.
* **EMIT-66** A tree that is not core is the error `not-core`, and its detail names the first node that offends.
* **EMIT-67** A `print` is `unsupported`: no target writes one yet.

## Names

* **EMIT-14** Every name an emitter writes comes from the entry point's mint ([CHK-104](checking.md#the-flat-tree)).
* **EMIT-15** Each target has a list of **reserved words**: its keywords and its predeclared types, together with every name a builtin writes in that target (EMIT-74).
  That is the function a call is written as, and every name a writer of its own calls, declares or reaches into, which its record lists per target.
* **EMIT-16** The reserved words of the target are taken in the mint before anything else is minted.
* **EMIT-17** A struct, a binding or a local whose name is reserved in a target is minted from that name and a trailing underscore, in that target only.
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
* **EMIT-23** A struct is declared after every struct it holds.
* **EMIT-106** A field of type `void` holds nothing, so no target declares it, and a construction writes no argument for it.
* **EMIT-107** A struct whose every field is `void` is `unsupported`, since it would be a struct of no member, which WGSL has no spelling for.
* **EMIT-118** An array is `array<T, N>` in WGSL and MSL, and in HLSL its element type with the lengths after the declared name, `float weights[3]`.
  A square literal is WGSL's constructor, and in HLSL and MSL a local assigned element by element, as a struct is (EMIT-55).
* **EMIT-119** Each member of a `@workgroup` binding is a variable of its own, minted `<binding>_<member>`: `groupshared` in HLSL and `var<workgroup>` in WGSL at file scope,
  and `threadgroup` at the top of the kernel in MSL, which has it nowhere else.
  It takes no group, so the groups after it in a list keep their numbers.
* **EMIT-120** An atomic is `atomic<u32>` in WGSL and `atomic_uint` in MSL, updated by `atomicAdd(&a, v)` and `atomic_fetch_add_explicit(&a, v, memory_order_relaxed)`.
  HLSL declares the plain integer, and its `Interlocked*` gives the value before through an out parameter, so a local declared ahead of the statement holds it.
  A load is `InterlockedOr` with 0, and a store `InterlockedExchange`, so that no plain access races an update.
* **EMIT-131** A barrier is the target's own execution barrier with one kind of memory, and MSL's one `threadgroup_barrier` names it by its `mem_flags`.
  `workgroup_barrier` is `GroupMemoryBarrierWithGroupSync` in HLSL, `workgroupBarrier` in WGSL and `mem_threadgroup` in MSL.
  `storage_barrier` is `DeviceMemoryBarrierWithGroupSync`, `storageBarrier` and `mem_device`.
  `texture_barrier` is `DeviceMemoryBarrierWithGroupSync`, `textureBarrier` and `mem_texture`.
* **EMIT-121** A binding array is its element's declaration with the length after the name, at its first slot, and the resources after it start `count` slots later.
  HLSL wraps an index marked `nonuniform` in `NonUniformResourceIndex`, which DXC carries into SPIR-V as `NonUniform`.

| SGL | HLSL | WGSL | MSL |
|---|---|---|---|
| `float` | `float` | `f32` | `float` |
| `float3`, `vec3`, `pos3` | `float3` | `vec3f` | `float3` |
| `float4`, `hpos4` | `float4` | `vec4f` | `float4` |
| `mat4` | `float4x4` | `mat4x4f` | `float4x4` |
| `int` | `int` | `i32` | `int` |
| `bool` | `bool` | `bool` | `bool` |

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
* **EMIT-123** HLSL writes the stages as dx12 and vulkan both take them:
  * a geometry stage is `[maxvertexcount(N)]` over `void`, taking `triangle T tri[3]` and last `inout TriangleStream<T>`, and `emit` and `end_strip` are `Append` and `RestartStrip`;
  * a control stage is a passthrough hull function that returns `patch[point_index]`, named by its `domain`, `partitioning`, `outputtopology`, `outputcontrolpoints` and `patchconstantfunc`;
  * the SGL body is that patch-constant function, `<name>_patch`, taking the `InputPatch`;
  * an evaluation stage is `[domain(…)]` over the function, taking the `const OutputPatch`, the patch constants, and `SV_DomainLocation`;
  * in patch constants, `@edge_factors` is `SV_TessFactor` and `@inside_factors` `SV_InsideTessFactor`;
  * the other members of patch constants take the locations after the control point's, so vulkan's two never collide.
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
* **EMIT-100** WGSL writes a 1D texture or image as a 2D one and a 1D array as a 2D array, since sg's webgpu backend creates every 1D texture that way (the bindings file, "Shapes").
* **EMIT-101** A call of a builtin that gives nothing is a statement as it stands, with no `_ =` in WGSL.
* **EMIT-102** A builtin a target cannot write as one expression declares a helper function ahead of the entry point, once per text, and the call names it.
  HLSL's `GetDimensions` writes through out parameters, so `size` is an overload of `sgl_size` per texture type the entry point passes.
* **EMIT-103** *Retired:* WGSL text no longer switches Tint's derivative uniformity analysis off wholesale, since the check pass refuses what is unsound (CHK-282).
  Where Tint refuses a program the pass accepts, the text silences Tint for that program alone; no such program is known yet.
* **EMIT-104** A resource at slot i of group N is `register(<class>i, spaceN)` in `hlsl-dx12` and `[[vk::binding(i, N)]]` in `hlsl-vulkan`, which is the address sg's backends give that slot.
  The class is `b` for the constant buffer, `u` for an image and a `mut` buffer, `s` for a sampler, and `t` for every other resource.
* **EMIT-105** An entry point that lists more than three groups is `too-many-groups` on every target, since sg binds three besides the inline constants.
  Only the entry point's list counts: a function that is no entry point takes the groups its caller hands it, which are no addresses of their own.
* **EMIT-133** A file-scope sampler the entry point's code reaches is one global named as the sampler is, and one the code does not reach is not declared.
  Its index i is its position among the module's file-scope samplers in declaration order, so every entry point and every stage states the same one.
  It stands where sg binds a pipeline layout's static sampler of index i: `register(s<i>, space10)` in `hlsl-dx12`, and `[[vk::binding(i + 1, 3)]]` in `hlsl-vulkan`.
  WGSL writes it as `@group(3) @binding(i + 1)`, since binding 0 of sg's own group is the inline constants', and MSL as the entry point's parameter `sampler name [[sampler(i)]]`.
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

## Matrices

* **EMIT-42** A matrix is column-major, and a vector stands to its right.
* **EMIT-43** HLSL declares every matrix member `column_major` and writes a product `mul(a, b)`; WGSL writes `a * b`, for a matrix times a vector and for a matrix times a matrix.
* **EMIT-44** `transform_position(m, p)` is the product of `m` and the four-vector `(p, 1.0)`.
* **EMIT-45** `transform_direction(m, v)` is the `xyz` of the product of `m` and `(v, 0.0)`.

## The text

* **EMIT-46** The text is meant to be read: real names, one statement per line, four spaces per level ([why](why/emitting.md#emit-46)).
* **EMIT-47** The text starts with a comment that names the stage, the entry point and the target.
* **EMIT-48** An immutable local is a named constant: `const T name = value;` in HLSL and `let name: T = value;` in WGSL.
* **EMIT-49** A float literal is the shortest decimal text that reads back as its value, always with a decimal point, and without a suffix.
* **EMIT-50** A literal that is infinite or not a number is `non-finite-literal`.
* **EMIT-51** An `@operator` builtin is its operator: `+`, `-`, `*`, `/`, `%`, the six comparisons, and the prefix `-`; the products of a matrix are EMIT-43.
  Parentheses follow the tree, and an operand of equal precedence on the right keeps them.
* **EMIT-125** An operand of `&`, `|`, `^`, `<<` or `>>` is parenthesized unless it is a name, a call, a literal or a prefix expression, since WGSL takes nothing else there.
  The operation itself is parenthesized wherever it is embedded.
  WGSL writes an `int` count as `u32(count)`, and MSL masks the count, `x << (count & 31)`, since a count past 31 is undefined there.
* **EMIT-126** A maths builtin is the target's function of its name, with these exceptions.
  `inverse_sqrt` is `rsqrt` in HLSL and MSL and `inverseSqrt` in WGSL; `fract` is `frac` in HLSL; `round` is `rint` in MSL, whose `round` is ties-away.
  `ddx` and `ddy` are `dpdx` and `dpdy` in WGSL and `dfdx` and `dfdy` in MSL.
  HLSL's `sign`, `countbits`, `firstbithigh` and `firstbitlow` give an int or a uint whatever they take, so the writer converts back, and `reversebits` of an int goes through a uint.
  WGSL writes a reinterpretation as `bitcast<T>`, MSL as `as_type<T>` and HLSL as `asuint`, `asint` or `asfloat`.
  MSL's `first_bit_*` and HLSL's packing functions are helpers the text declares (EMIT-102).
* **EMIT-127** A stage input is a parameter of the target's entry point, handed over as the target's own type, and converted to SGL's where the body reads it.
  HLSL marks it with a semantic (`SV_VertexID`, `SV_DispatchThreadID`, …), WGSL with `@builtin(…)` and MSL with an attribute (`[[vertex_id]]`, …).
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
* **EMIT-52** Every other builtin function is a call of the target's function of that name, and `mix` is `lerp` in HLSL.
* **EMIT-53** A construction of a builtin type is a call of the target's type, on one line: `float3(x, y, z)`, `vec3f(x, y, z)`.
* **EMIT-73** The operand of a prefix `-` that is no name, call or member stands in parentheses, so `-(-0.4)` never reads as a decrement.
* **EMIT-74** How a builtin is written is a field of its registry record: a call under a name per target, an infix or a prefix operator, or a writer of its own ([why](why/emitting.md#emit-74)).
  No emitter holds a list of builtins, and the size and alignment the layout rules and each target's own rule place a value by are fields of the type's record.
* **EMIT-75** A value that is evaluated and dropped is a statement of its own: `value;` in HLSL, `_ = value;` in WGSL, and `(void)(value);` in MSL ([why](why/emitting.md#emit-75)).
* **EMIT-54** A construction of a struct of the program is `name(a, b)` in WGSL.
* **EMIT-55** In HLSL it is a local that is declared and then assigned member by member; a returned one is minted from `result` ([why](why/emitting.md#emit-55)).
* **EMIT-68** Control flow is written by [the table of the core form](legalization.md#the-table), and a nested body is one level deeper.
* **EMIT-69** The C-like targets put a brace on a line of its own and a condition in parentheses; WGSL puts the brace behind the head and writes the condition bare.
* **EMIT-70** An `int` literal is its decimal text, and the one that does not fit behind a minus is `(-2147483647 - 1)`.
* **EMIT-71** A `while` whose condition builds a struct member by member is written as a loop that tests at its top, so the struct is built before every test.
* **EMIT-78** A `switch` is the target's own, by the table of the core form, and a label is the constant of EMIT-76 where its value is a case and its decimal text otherwise.
* **EMIT-79** In the C-like targets the emitter ends each arm with `break;`, and writes none after a body that already exits.
* **EMIT-80** WGSL writes an arm's values as one comma list, and the C-like targets as one label per value.

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

## MSL

The rules above say HLSL and WGSL by name; these say what `msl` writes in the same places.

* **EMIT-56** After the comment of EMIT-47, the text is `#include <metal_stdlib>`, `using namespace metal;` and an empty line.
* **EMIT-57** MSL's reserved words also hold every name the Metal toolchain declares at global scope or in `metal`, its macros included, and `main` ([why](why/emitting.md#emit-57)).
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
  An acceleration member is `RaytracingAccelerationStructure` in HLSL and `instance_acceleration_structure` in MSL, and WGSL declares none: it keeps its slot and takes no binding.
  An entry point whose code reads sg's acceleration pool declares it and the dispatch's roots in sg's reserved group, beside the inline constants:
  `@group(3) @binding(17) var<storage, read> sg_acceleration_pool: array<vec4u>;` and `@group(3) @binding(18) var<uniform> sg_acceleration_roots: array<vec4u, 4>;`.
  The root of the entry point's k-th acceleration member (CHK-325) is `sg_acceleration_roots[k / 4][k % 4]`.
  So WGSL traces the first 16 acceleration members, and a trace of a later one is `too-many-acceleration-structures`.
* **EMIT-136** HLSL writes a ray-tracing stage as a library export named by its kind, `[shader("closesthit")]` over `void`, whose payload is an `inout` parameter.
  A triangle closest hit or any hit takes `BuiltInTriangleIntersectionAttributes` too.
  An any hit's decision ends the stage: `ignore` is `IgnoreHit()`, `accept_and_end_search` `AcceptHitAndEndSearch()`, and `accept` a plain `return`.
  A trace of a ray type is `TraceRay` over a `RayDesc`, and a callable's call is `CallShader`.
* **EMIT-137** A payload is a `[raypayload]` struct, and each field states which stages read and write it, `read(caller, closesthit) : write(miss)` ([why](why/emitting.md#emit-137)).
  They are inferred from every entry point of the module: a stage reads and writes what its payload parameter does, and a caller what the local it traces with does.
  A stage that writes a field reads it too, since a write on some paths keeps the rest, and whatever some stage writes the caller reads, and the reverse.
  A payload type that a pipeline with `.host` groups traces, or that no pipeline of the module traces, states the widest access instead.
  An intersection's report is `ReportHit(t, 0, attributes)` where it hits, and a procedural record's shaders take the attributes as a parameter of their own.
* **EMIT-139** MSL writes a pipeline as a kernel that intersects and then calls through sg's tables ([why](why/emitting.md#emit-139)).
  Every function of the pipeline agrees on the shapes below, since each is compiled apart.
  * The raygen is a `kernel`, which takes the argument buffers, sg's tables at `[[buffer(3)]]` and each instance's hit-group offset at `[[buffer(5)]]`.
  * A miss, a closest hit and a callable are `[[visible]]` functions of one signature: the payload as `uint4` words, a hit record but for a callable, and an `sgl_context`.
    The context carries the argument buffers, the inline constants, the tables and the launch, and the function binds each to a local.
  * An any hit is an `[[intersection(triangle, triangle_data, instancing)]]` function that answers whether it accepts, and writes the payload back into the ray data.
    `accept_and_end_search` is `accept` there.
  * A procedural record is its traversal entry point (CHK-345), an `[[intersection(bounding_box, triangle_data, instancing)]]` function.
  * A trace is the kernel's own `intersector`, configured from the ray flags, then the miss or the closest hit through the tables.
    The closest hit's record is the instance's offset plus the geometry index times the multiplier plus the contribution, as DXR finds it.
  * The ray data holds as many words as the largest payload of the set, so every shader of a pipeline agrees on it.
  * A hit's instance transforms are the identity, since Metal hands them only under intersection tags sg's tables do not declare.

## Layout

Every value in GPU memory — a constant block or a buffer's element — is placed by one rule per address space, the same on every target.
The C++ struct a package generates is that layout byte for byte, padding included, so the host copies it in as it is.
**That struct is the only thing the host may rely on**: without an annotation, where a member lands is the compiler's choice (EMIT-116).
EMIT-110 and EMIT-111 describe today's choice, not a promise.

* **EMIT-110** A constant block, a group's plain members or an `@inline` binding, is placed by HLSL's constant-buffer packing.
  It is read in rows of 16 bytes, and a value that would cross a row starts the next one.
  A `float4`, a matrix and a nested struct start a row, and what follows a nested struct packs against its last member.
* **EMIT-111** A buffer's element is placed by dx12's structured-buffer packing: each value right behind the one before, every one 4-byte aligned.
  A buffer strides by its element's size, which is where its last value ends.
* **EMIT-112** Each target is made to follow the two rules ([why](why/emitting.md#emit-112)).
  `hlsl-dx12` writes nothing, since they are its own rules.
  `hlsl-vulkan` states every offset, which sg's vulkan backend admits by requiring `scalarBlockLayout`.
  WGSL and MSL write a root as its memory form wherever their own rule would place one of its values elsewhere.
* **EMIT-113** A memory form is one struct of the root's builtin values in memory order, with padding fields where the layout leaves room.
  A value the target places at its offset natively is a field of its own type.
  A vector that it does not is split into scalar fields in WGSL, and is its `packed_` type in MSL; a matrix is split into its scalars.
  A read of a value rebuilds it from its fields, and a write stores each field; a whole struct is stored once into a local, then field by field.
* **EMIT-114** A struct placed both in a constant block and in a buffer's element would have two layouts, and is `layout-conflict`.
* **EMIT-115** `@no_padding` on a struct or on a binding makes a gap before any of its members `padding-forbidden`, and the detail says where.
  The rest of a block's last row follows no member, so it is no gap.
* **EMIT-116** A layout carries no guarantee without an annotation that asks for one ([why](why/emitting.md#emit-116)).
  The compiler may place members in another order than they are declared, to pack them tighter.
  Host code reaches GPU memory through the generated struct, never through offsets or an order it assumed.

## Error kinds

| kind | reported by |
|---|---|
| `module-has-errors` | EMIT-10 |
| `unknown-entry-point` | EMIT-11 |
| `unsupported` | EMIT-12, EMIT-33, EMIT-34, EMIT-38, EMIT-39, EMIT-67, EMIT-81, EMIT-107 |
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
| `target-lacks-feature` | EMIT-109 |

## Open

* GLSL, which comes through the same seam.
* Arrays in GPU memory, which the checker refuses today (CHK-291).
  In a constant block every element starts a row, as HLSL places it: an element shorter than a row is `array<vec4f, N>` read through `.x` in WGSL, and `slib::row<T>` on the host.
* An annotation that fixes a layout, `@layout(.hlsl)` or `@layout(.cpp)`, for memory a host fills without the generated struct; until it exists, EMIT-116 says nothing is fixed.
* `mat3`, which SGL has no type for yet: its three columns each start a row in a constant block (44 bytes), it is 36 bytes in a buffer's element, and its columns split in a memory form.
  The host holds a block's as `slib::gpu_mat3` and a buffer's as `tg::mat3f`, which is those 36 bytes.
* Whether an emit error becomes a diagnostic with a span; today it names a symbol and carries a detail.
* Whether the size of an inline block has to agree between targets as its offsets do; WGSL rounds it up to 16 bytes.
* How a vertex input's dx12 semantic is chosen once a member wants one that is not its name.
* How a splatted value reads once a target can take the vector whole ([checking](checking.md#open)).
* Whether the reserved words of a target hold every function of that target, or only the ones a builtin is written as.
