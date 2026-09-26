# SGL Bindings

A **binding group** is what a shader needs from outside: the buffers, textures, images and samplers a host binds before a dispatch or a draw.
This file is the model — what a group is, what may stand in one, and how it reaches a target.
The rules live where every other rule does.
[AST-73 to AST-76](syntax/ast.md#bindings-and-samplers) is the syntax.
[CHK-40 to CHK-46](semantics/checking.md#bindings) is what a group means.
[EMIT-82 onward](semantics/emitting.md#bindings) is what a target sees.
Back to the [specification](_index.md).

## A group and its members

```sgl
binding post:
    texel_size: float2
    src: texture_2d[float4]
    dst: out image_2d[.rgba8_unorm]
    sampler bilinear:
        filter = .linear
        address = .clamp_edge
```

A member is a name and a type, like a struct field.
What kind of resource it is follows from that type, and nothing else marks it.

**A member of a plain value type is a constant**, and the group's plain members together are one constant buffer the compiler builds.
So `texel_size: float2` above costs no declaration of its own: the group has an implicit constant buffer, and `texel_size` is a field of it.
That is the common case, and it is why `constants[T]` below is rarer than it looks.

**A member of a resource type is that resource.**
The table is the one sg already commits to.
`sg::binding_type` in [binding.hh](../../../shaped-graphics/src/shaped-graphics/binding/binding.hh) is the portable set every backend maps to, and SGL spells it rather than deciding it again.

| SGL | `sg::binding_type` | `sg::access_mode` | what it is |
|---|---|---|---|
| a plain value type | `constants_buffer` | `read` | a field of the group's implicit constant buffer |
| `constants[T]` | `constants_buffer` | `read` | a constant buffer that comes from somewhere else, already laid out |
| `buffer[T]` | `buffer` | `read` | an array of `T` the shader reads |
| `mut buffer[T]` | `buffer` | `read_write` | an array of `T` the shader reads and writes |
| `bytes` | `bytes` | `read` | raw bytes, addressed by offset |
| `mut bytes` | `bytes` | `read_write` | raw bytes the shader also writes |
| `texture_2d[T]` and its neighbours | `texture` | `read` | a texture the shader samples or loads |
| `texture_2d_depth` and its neighbours | `texture` | `read` | a depth texture, sample type `depth` |
| `image_2d[.F]` and its neighbours | `image` | `read` | a storage texture the shader only reads |
| `mut image_2d[.F]` | `image` | `read_write` | a storage texture the shader reads and writes |
| `out image_2d[.F]` | `image` | `write` | a storage texture the shader only writes |
| `sampler`, `comparison_sampler` | `sampler` | `read` | a sampler the host binds |
| `sampler name:` with settings | a static sampler of the group's layout | — | a sampler nobody binds |

**sg and SGL name the same two things: the resource, and what the shader does with it.**
The type is the kind, and the access word is `sg::access_mode`, so every row above is one-to-one.

## Access

Three spellings, and a resource takes the ones it has.

* Unmarked — the shader only reads it.
* `mut` — the shader reads and writes it.
* `out` — the shader only writes it.

**A buffer is never `out`.**
No target has a write-only buffer, and WGSL refuses one in as many words: *access mode 'write' is not valid for the 'storage' address space*.
**A texture is never `mut` or `out`**, since a sampled texture is read-only on every target.
An image has all three, exactly as `sg::access_mode` has three.

The asymmetry is WebGPU's rather than ours.
Core WebGPU allows read-write storage only for the `r32` formats, so an image of any other format is unmarked or `out` there, and `mut` needs a feature ([Features](#features)).
dx12 and vulkan do not ask — a UAV is read-write to them whatever the shader said.

## Textures and images

**A sampled texture and a storage texture are two families, because every target gives them different type arguments.**
A sampled texture goes through the texture unit and shows the shader a component type: `texture_2d[float4]`.
A storage texture is addressed per texel, and the shader has to name its memory format: `image_2d[.rgba8_unorm]`.
WGSL spells the format inside the type, `texture_storage_2d<rgba8unorm, write>`, and vulkan wants it for a storage image read without `shaderStorageImageReadWithoutFormat`.
HLSL ignores it, so carrying it costs dx12 nothing.

"image" is Vulkan's and GLSL's word for a storage texture, and the more widely shared of the candidates.

**One family was the alternative, and it lost.**
`texture_2d` alone would take a component type when unmarked and a format under `mut` or `out`.
The argument's kind would then depend on the word to its left, and a read-only storage texture would have no spelling at all.

### Shapes

Every shape sg's `texture_view_dimension` has is a type.
Each is spelled as sg spells its texture of that shape, `texture_2d` for `sg::texture_2d`, so the shader and the host say the same word.
The digit is set off by `_`, unlike a vector's `float4`, because it names a dimension rather than a count.

| sg `texture_view_dimension` | texture | depth texture | image |
|---|---|---|---|
| `tex_1d` | `texture_1d` | — | `image_1d` |
| `tex_1d_array` | `texture_1d_array` | — | `image_1d_array` |
| `tex_2d` | `texture_2d` | `texture_2d_depth` | `image_2d` |
| `tex_2d_array` | `texture_2d_array` | `texture_2d_array_depth` | `image_2d_array` |
| `tex_3d` | `texture_3d` | — | `image_3d` |
| `cube` | `texture_cube` | `texture_cube_depth` | — |
| `cube_array` | `texture_cube_array` | `texture_cube_array_depth` | — |
| `tex_2d_ms` | `texture_2d_ms` | `texture_2d_ms_depth` | — |
| `tex_2d_ms_array` | `texture_2d_ms_array`, feature only | — | — |

No target has a cube or a multisampled storage texture, and no target has a 1D or 3D depth texture.
A depth texture is its own type, with the sg shape and then `_depth`, because it has functions of its own: comparison sampling.

**1D on webgpu is a polyfill.**
sg's webgpu backend creates every 1D texture as a 2D one, one texel high, and so does the WGSL SGL writes.
One rule is simpler than "1D without mips, 2D with them", removing 1D would remove it everywhere, and drivers are understood to promote 1D the same way.
A 1D texture's size then reads its width from `x`.

### Sample types

WebGPU wants a sampled texture's sample type on the layout before any texture is bound, and refuses a mismatch; dx12 and vulkan never ask.
`sg::texture_sample_type` is the set, and SGL states every value of it in the declaration:

* **The component type** gives three: any `float` width is `filterable_float`, any `int` width is `sint`, any `uint` width is `uint`.
  The width is free, from 1 to 4, and decides what a sample returns.
* **A depth type** gives `depth`.
* **`@unfilterable` on the member** gives `unfilterable_float`.
  Core WebGPU makes `r32_float`, `rg32_float` and `rgba32_float` unfilterable, and a multisampled texture is unfilterable by definition, needing no attribute.

```sgl
binding lighting:
    albedo: texture_2d[float4]
    ids: texture_2d[uint]
    shadow: texture_2d_depth
    @unfilterable positions: texture_2d[float4]
```

SGL never sees a sampled texture's format, so it cannot know that a view bound later is 32-bit float.
That refusal is sg's: binding such a view to a `filterable_float` layout is an error on every backend unless the device has the feature ([Features](#features)).

### Image formats

**An image's argument is a value of sg's format enum**, spelled as an enum case: `image_2d[.rgba8_unorm]`.
A format is not a type, and its name is sg's `sg::pixel_format` name, so the shader, the generated host code and every diagnostic say the same word.
The WGSL writer maps it to WGSL's spelling (`rgba8unorm`, and `rg11b10ufloat` for `rg11b10_float`).

The portable formats are core WebGPU's image formats:
`rgba8_unorm`, `rgba8_snorm`, `rgba8_uint`, `rgba8_sint`, `rgba16_uint`, `rgba16_sint`, `rgba16_float`, and the `r32`, `rg32` and `rgba32` formats in `float`, `uint` and `sint`.
Every other image format needs a feature.

## Samplers

Three forms, and each lands in a different place of sg's layout model.

```sgl
sampler linear_clamp:
    filter = .linear
    address = .clamp_edge

binding material:
    albedo: texture_2d[float4]
    sampler albedo_smp:
        filter = .linear
        max_anisotropy = 8
    user_smp: sampler
    shadow_smp: comparison_sampler
```

* **`sampler name:` at file scope** is a static sampler of the pipeline layout, an `sg::bound_sampler`.
  Nobody binds it, so it is not listed anywhere: it joins the layout of every entry point whose inlined body uses it, and no other.
* **`sampler name:` inside a binding** is a static sampler of that group's layout, an `sg::named_sampler`.
  It is part of the group, and the host binds nothing for it.
  An `@inline` binding holds constants only, so a sampler in one is a normal error rather than a sampler moved elsewhere.
* **`name: sampler` as a member** is a dynamic sampler, which the host binds like any other resource.
  `sampler` is a keyword, and in a type position that keyword denotes the sampler type.

**A dynamic sampler's kind is part of its declaration**, because WebGPU wants it on the layout.
`sampler` is `filtering`, `comparison_sampler` is `comparison`, and `@non_filtering` on a `sampler` member is `non_filtering` — the only kind an `@unfilterable` texture may be sampled through.
A static sampler's kind needs no spelling: it follows from its settings, `comparison` when `compare` is set, and `non_filtering` when every filter is nearest.

**A sampler's settings** are one `name = value` per line ([AST-76](syntax/ast.md#bindings-and-samplers)), and each sets a field of `sg::sampler`.
They are the names slib's `#pragma sc static` takes in hand-written HLSL, so both languages say the same thing.
They apply in order, so a later setting overrides what an earlier one set, `filter` included:

| setting | sets |
|---|---|
| `filter` | `min_filter`, `mag_filter` and `mip_filter` at once |
| `min_filter`, `mag_filter`, `mip_filter` | one of them: `.nearest` or `.linear` |
| `address` | `address_u`, `address_v` and `address_w` at once |
| `address_u`, `address_v`, `address_w` | one of them: `.repeat`, `.mirror_repeat` or `.clamp_edge` |
| `compare` | the compare op, which makes it a comparison sampler: `.less`, `.less_equal`, … |
| `max_anisotropy` | an int from 1 to 16, where 1 is off; above 1 every filter is `.linear`, since WebGPU refuses anything else |
| `min_lod`, `max_lod`, `mip_lod_bias` | the mip clamp and bias |

## Features

**SGL refuses a non-portable form by feature, never by target.**
A form that some backend lacks is a normal error on every target, unless the function opts into the feature that grants it.
Opting in is what makes a shader non-portable on purpose, and the host then asks the device before it builds a pipeline.
The opt-in itself is unbuilt ([feature-levels.md](incubator/feature-levels.md)), so today each of these is refused with a diagnostic naming its feature.

| form | the feature that grants it |
|---|---|
| `texture_2d_ms_array` | multisampled arrays, which WebGPU lacks |
| `mut image*[.F]` with `F` not `r32_float`, `r32_uint` or `r32_sint` | `sg::feature::readwrite_image_formats`, WebGPU's `texture-formats-tier2` |
| `image*[.F]` with `F` outside the portable image formats | the tier-1 image formats, WebGPU's `texture-formats-tier1` |
| filtering a 32-bit float texture | float32 filtering, WebGPU's `float32-filterable`; refused by sg at bind time |

## Which group a binding is

**A binding block carries no number.**
The group number is the position of that binding in the entry point's binding list:

```sgl
@compute(64) fun cs(t: dispatch){work} : …
```

compiles `work` as group 0.
An entry point listing `{frame, work}` compiles the same `work` as group 1, and neither declaration changed.

This is scoped the way it has to be.
A shader file is a library: it holds every binding its functions use, and an entry point needs a few of them.
A number on the declaration would fix a layout for the whole file, and two entry points wanting different layouts could not share one.

**The list is a layout, not a use list.**
A binding an entry point lists but never reads still takes its position, because the host binds by position, and nothing reports it.

**`@inline` constants stand last.**
They are listed like any other binding and skipped when numbering, since sg addresses them itself.
An `@inline` binding anywhere but the last position of a list is a normal error, so that reading order matches binding order.

## How a group reaches sg

**The slots of a group are its members in declaration order**, after the constant block at slot 0 when the group has plain members.
A group-scope static sampler takes a slot like any sampler, since sg matches it by name to a sampler binding.

**SGL states every fact of a binding itself.**
Dimension, sample type, image format, access and sampler kind are in the declaration, and `sgl describe` hands each to the host.
The generated group's table is what the host builds its layout from, so that is where every fact reaches sg.
A compiled shader only has to fit that layout, and sg's fit check compares a binding's name, slot, count and kind — never the facts beyond them.
The WGSL SGL writes states all of them, which a test holds to the generated table; HLSL states the dimension, and the vulkan text an image's format as `[[vk::image_format]]`.
HLSL cannot say `unfilterable` at all, which costs nothing, since dx12 and vulkan read none of it.
Building the layout by reflecting the WGSL instead was declined: that text is written from the same declaration, so reading it back is `sgl describe` with a parser in between.
And `texture_2d<f32>` fits a filterable and an unfilterable layout alike, so the reflection could not even recover `@unfilterable`.

**MSL, as it is intended.**
sg's metal backend makes a group one argument buffer at `[[buffer(group)]]`, whose member `[[id(n)]]` is slot `n` of the group.
A texture or sampler slot holds a resource id, so a group reads in MSL as:

```cpp
struct post_bindings
{
    constant post_data* post [[id(0)]];
    texture2d<float, access::sample> src [[id(1)]];
    texture2d<float, access::write> dst [[id(2)]];
    sampler bilinear [[id(3)]];
};
kernel void main0(constant post_bindings& post_group [[buffer(0)]], uint3 id_in [[thread_position_in_grid]])
```

Every call is inlined, so resources are parameters of the entry point alone.
An image's access maps one-to-one onto `access::read`, `access::write` and `access::read_write`, and a depth texture is `depth2d<float>`.
A file-scope static sampler can be a `constexpr sampler` in the text.

## What the compiler carries today

The syntax above is what the AST builds; the check pass is what limits it.
Everything not named here is the diagnostic `unsupported-yet`, never a guess.

* `buffer[T]` and `mut buffer[T]`, for a `T` that is a scalar or a vector.
* A subscript on a buffer, as a value and as the place of an assignment.
* Every texture, depth texture, image and sampler form above, with `@unfilterable` and `@non_filtering`.
* A static sampler in a binding, and the `needs-feature` refusals.
* A texture, an image or a sampler handed to a builtin, which is the only way one is used ([CHK-206](semantics/checking.md#bindings)).
  The builtins that take one are `sample`, `load`, `store` and `size` in `prelude/builtins.sgl`, called as methods of it: `tex.sample(uv, smp)`.
* A plain member of a group, as a field of the constant buffer the group owns, for a type whose place in a block every target agrees on.
* The positional group numbering, and `@inline` last.
* A resource's host name, its path `binding.member` ([CHK-171](semantics/checking.md#bindings)), which the text reports beside the identifier it minted.

Three targets write a group, and the fourth declines rather than guessing.
WGSL gives each resource its own `@group`/`@binding`, and HLSL declares each at file scope with `register(<class>slot, spaceN)` on dx12 and `[[vk::binding(slot, N)]]` on vulkan.
A group's plain members are one constant buffer at the group's slot 0, named after the binding, and its resources follow it in declaration order.
MSL declines every group until slib has a compiler that turns its text into a metallib.

A struct element type, `bytes`, `constants[T]` and a file-scope `sampler` all parse and are then reported.
A file-scope sampler waits for slib to carry a pipeline layout's static samplers, which it has no spelling for yet.
That is deliberate.
The shape is decided, so it is written down here and the AST constructs it.
A shader using one then gets a diagnostic that names the feature, rather than a parse error that names nothing.

## Open

* `values.length` — WGSL, HLSL and SPIR-V can all answer it without packing data, and MSL cannot: a Metal buffer argument is a pointer and carries no length.
  Until metal is a target with a compiler behind it, a shader passes the count in.
* The layout of a struct element type: our own rules, portable across the four targets, with explicit padding where they need it, and the generated host struct matching byte for byte.
* A buffer as a field of a struct, which is `unsupported-yet` like every buffer outside a binding member.
  It could be allowed where the buffer is hoisted and stays uniform across every use, a scalarization of the struct that inlining makes possible.
* The functions over textures and images — sampling, loads, stores and sizes — and a default sampler on a texture member ([texture-methods.md](incubator/texture-methods.md)).
* Arrays of textures and images, which need `sg::feature::binding_arrays` and so the feature opt-in.
