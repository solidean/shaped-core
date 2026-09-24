# Emitting

*Tracer: deliberately thin.*

An emitter writes the text a graphics API compiles, from one flat tree of a [checked module](checking.md#the-flat-tree).
It carries what [cube.sgl](../../../tests/samples/cube.sgl) needs, and compute entry points, groups, buffers, enums and `case` besides; every other construct is `unsupported`.
The cube's text has met its readers: DXC compiles both HLSL targets, and WebGPU compiles the WGSL.
[sgl-cube](../../../../../../examples/graphics/sgl-cube/sgl_cube.cc) draws the same picture on all three, and no rule below had to change for it.
**The MSL text has not been through a Metal compiler yet**: its rules are pinned as text, and nothing has compiled or drawn with it.
Back to the [semantics](_index.md); the reasons are in [why/emitting.md](why/emitting.md).

## Targets

* **EMIT-1** A **target** is a text format together with the addressing rules of the backend that reads it.
* **EMIT-2** The targets are `hlsl-dx12`, `hlsl-vulkan`, `wgsl` and `msl` ([why](why/emitting.md#emit-2)).
* **EMIT-3** The WGSL and MSL text carries its final addresses: no later pass numbers a binding, a location or an offset.
  The HLSL text carries every location and offset and the group of each resource, and leaves the register to slib's binding pass (EMIT-86); HLSL with final registers is open.
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
* **EMIT-13** No error depends on the target but EMIT-89's: an entry point is written for every target or for none ([why](why/emitting.md#emit-13)).
  The exception is `msl`, which refuses what a Metal entry point takes as an argument.
* **EMIT-66** A tree that is not core is the error `not-core`, and its detail names the first node that offends.
* **EMIT-67** A `print` is `unsupported`: no target writes one yet.

## Names

* **EMIT-14** Every name an emitter writes comes from the entry point's mint ([CHK-104](checking.md#the-flat-tree)).
* **EMIT-15** Each target has a list of **reserved words**: its keywords and its predeclared types, together with every function name a builtin is written as in that target (EMIT-74).
* **EMIT-16** The reserved words of the target are taken in the mint before anything else is minted.
* **EMIT-17** A struct, a binding or a local whose name is reserved in a target is minted from that name and a trailing underscore, in that target only.
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
* **EMIT-95** The text of an entry point comes with the pair of every buffer and constant buffer it declares: the name it minted, and the host name CHK-171 gives ([why](why/emitting.md#emit-95)).

| stage | parameter | result |
|---|---|---|
| `@vertex` | vertex input | stage link |
| `@pixel` | stage link | render targets |

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
* **EMIT-38** A second `@inline` binding is `unsupported`, and so is one that is not the last of the list.
* **EMIT-39** A member of an `@inline` binding is of a builtin type whose record has a size in a block, which `bool` has not, or it is `unsupported`.
* **EMIT-40** A member's offset follows HLSL's packing of a constant buffer, and `hlsl-vulkan` states it on every member ([why](why/emitting.md#emit-40)).
* **EMIT-41** A member that WGSL's layout or MSL's places at another offset is `layout-mismatch`, and its detail gives the offset in each.

| target | the global |
|---|---|
| `hlsl-dx12` | `ConstantBuffer<T> name : register(b0, space9);` |
| `hlsl-vulkan` | `[[vk::push_constant]] ConstantBuffer<T> name;` |
| `wgsl` | `@group(3) @binding(0) var<uniform> name: T;` |
| `msl` | no global: the parameter `constant T& name [[buffer(4)]]` (EMIT-58) |

A binding that is not `@inline` is a group.

* **EMIT-82** A group's number is its position in the entry point's binding list, the `@inline` binding skipped.
* **EMIT-83** A group's plain members are a struct of their own and one constant buffer of it, named after the binding, at slot 0.
* **EMIT-84** A group's plain members are placed by EMIT-39 to EMIT-41, as the members of an `@inline` binding are.
* **EMIT-85** A buffer member is one global whose name is minted from `<binding>_<member>`, and a group's constant buffer is named after the binding, as any declaration is.
* **EMIT-86** HLSL writes a group as `#pragma sc group N` and a namespace `<binding>_bindings`, with no register: slib's binding pass assigns every one ([why](why/emitting.md#emit-86)).
* **EMIT-87** The struct of a group's constant buffer stands ahead of that namespace, and `hlsl-vulkan` states no offset on its members ([why](why/emitting.md#emit-87)).
* **EMIT-88** WGSL writes each resource of a group as `@group(N) @binding(slot)`: the constant buffer as `var<uniform>`, a buffer as a `var<storage>` array, `read` or `read_write`.
* **EMIT-89** MSL writes no group and no compute entry point yet: an entry point that lists a group, or is `@compute`, is `unsupported`.
  How a group will read in MSL is in [bindings.md](../bindings.md#how-a-group-reaches-sg).
* **EMIT-90** A group's resources — buffers, textures, images and samplers — take the slots after its constant buffer, in declaration order, from 1, or from 0 when it has no plain member.
* **EMIT-96** A texture, an image and a sampler member are each one global minted as a buffer's is, by EMIT-85, and each has its target's own type by the table below.
* **EMIT-97** `hlsl-vulkan` states an image's format as `[[vk::image_format]]`, which DXC turns into a typed SPIR-V image; an image whose format SPIR-V lacks, `bgra8_unorm`, states none.
* **EMIT-98** A static sampler of a group is its `SamplerState` preceded by slib's `#pragma sc static`, which carries every filter and address and each other setting that is not its default.
  WGSL has no static sampler, and writes it as it writes a sampler the host binds: the layout says it is static.
* **EMIT-99** WGSL writes a 1D texture or image as a 2D one and a 1D array as a 2D array, since sg's webgpu backend creates every 1D texture that way (the bindings file, "Shapes").
* **EMIT-100** A call of a builtin that gives nothing is a statement as it stands, with no `_ =` in WGSL.

| SGL | HLSL | WGSL |
|---|---|---|
| `texture2d[float4]` | `Texture2D<float4>` | `texture_2d<f32>` |
| `texture2d_depth` | `Texture2D<float>` | `texture_depth_2d` |
| `image2d[.rgba8_unorm]` | `RWTexture2D<float4>` | `texture_storage_2d<rgba8unorm, read>` |
| `out image2d[.r32_float]` | `RWTexture2D<float>` | `texture_storage_2d<r32float, write>` |
| `sampler`, `comparison_sampler` | `SamplerState`, `SamplerComparisonState` | `sampler`, `sampler_comparison` |

The other shapes follow the same pattern: HLSL's `Texture2DArray`, `TextureCube`, `Texture2DMS`, and WGSL's `texture_2d_array`, `texture_cube`, `texture_multisampled_2d`.
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
* **EMIT-51** An `@operator` builtin is its operator: `+`, `-`, `*`, `/`, the six comparisons, and the prefix `-`; the products of a matrix are EMIT-43.
  Parentheses follow the tree, and an operand of equal precedence on the right keeps them.
* **EMIT-52** Every other builtin function is a call of the target's function of that name, and `mix` is `lerp` in HLSL.
* **EMIT-53** A construction of a builtin type is a call of the target's type, on one line: `float3(x, y, z)`, `vec3f(x, y, z)`.
* **EMIT-73** The operand of a prefix `-` that is no name, call or member stands in parentheses, so `-(-0.4)` never reads as a decrement.
* **EMIT-74** How a builtin is written is a field of its registry record: a call under a name per target, an infix or a prefix operator, or a writer of its own ([why](why/emitting.md#emit-74)).
  No emitter holds a list of builtins, and the size and alignment EMIT-40, EMIT-41 and EMIT-62 place a member by are fields of the type's record.
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
* **EMIT-57** MSL's reserved words also hold the types and the functions of its standard library, and `main` ([why](why/emitting.md#emit-57)).
* **EMIT-58** An `@inline binding` is a struct of its members and the parameter `constant T& name [[buffer(4)]]` of the entry point ([why](why/emitting.md#emit-58)).
* **EMIT-59** The entry point is a `vertex` or a `fragment` function, and its SGL parameter carries `[[stage_in]]`; a compute entry point is EMIT-89's.
* **EMIT-60** A member with `@position` is `[[position]]`.
* **EMIT-61** A member at location i is `[[attribute(i)]]` in a vertex input, `[[user(sgli)]]` in a stage link, and `[[color(i)]]` in a render target struct.
* **EMIT-62** In a block, MSL places `float3` at a multiple of 16 and gives it 16 bytes, and everything else as WGSL does ([why](why/emitting.md#emit-62)).
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

So `{float3; float}` is `layout-mismatch`: the `float` is at byte 12 in HLSL and in WGSL, and at byte 16 in MSL.

## Error kinds

| kind | reported by |
|---|---|
| `module-has-errors` | EMIT-10 |
| `unknown-entry-point` | EMIT-11 |
| `unsupported` | EMIT-12, EMIT-33, EMIT-34, EMIT-38, EMIT-39, EMIT-67, EMIT-81, EMIT-89 |
| `reserved-entry-point-name` | none: retired by EMIT-21 |
| `system-value-semantic` | EMIT-32 |
| `layout-mismatch` | EMIT-41 |
| `non-finite-literal` | EMIT-50 |
| `malformed-tree` | a flat tree the check pass does not produce |
| `not-core` | EMIT-66 |

## Open

* GLSL, which comes through the same seam.
* HLSL with final registers, one emission for dx12 and one for vulkan, so that no binding pass reads SGL's text and EMIT-3 holds for every target.
* HLSL with final registers, one emission for dx12 and one for vulkan, so that no binding pass reads SGL's text and EMIT-3 holds for every target.
* A Metal compiler for the MSL text, and the buffer index of EMIT-58, which sg's metal backend has yet to adopt.
* Whether a block member becomes `packed_float3` in MSL, which would let `{float3; float}` through at the price of a conversion on every read.
* Whether an emit error becomes a diagnostic with a span; today it names a symbol and carries a detail.
* Whether the size of an inline block has to agree between targets as its offsets do; WGSL rounds it up to 16 bytes.
* How a vertex input's dx12 semantic is chosen once a member wants one that is not its name.
* How a splatted value reads once a target can take the vector whole ([checking](checking.md#open)).
* Whether the reserved words of a target hold every function of that target, or only the ones a builtin is written as.
* A function that a custom writer of EMIT-74 calls, such as HLSL's `mul`, which is reserved by the target's list and not by the record.
