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
Where its members land is the compiler's choice, unless `@layout(.hlsl)` or `@layout(.cpp)` on the binding promises a layout ([CHK-369](semantics/checking.md#bindings)).
The host can then fill the block from its own struct.

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
| `acceleration_structure[.triangles]` and its two neighbours | `acceleration_structure` | `read` | the TLAS a trace runs against ([Features](#features)) |

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

**An image whose format the host picks names an option** ([CHK-353](semantics/checking.md#consts)).
Each format the host asks for is one compile, and the text names that format exactly as it names a written one, on every target and with no feature.

```sgl sketch
@option const output_format: pixel_format = .rgba16_float

binding outputs:
    upscaled: out image_2d[output_format]
```

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
  Its index in that layout is its position among the file's samplers, so every stage and every entry point states the same one.
  A stage holds 16, so an entry point reaching one of index 16 or more is `too-many-samplers`, however few it reaches ([EMIT-133](semantics/emitting.md#bindings)).
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

## Sampling

**A texture is read through its methods, which are builtins of the prelude called with the texture first.**
`frame.sky.sample(dir, smp)` is `sample(frame.sky, dir, smp)`, and every argument of a sample or a gather after the coordinate that is not the sampler is named.

| method | of | takes |
|---|---|---|
| `sample` | a texture of floats, and a depth texture | the level from derivatives, and only in a pixel stage |
| `sample(…, level = l)` | the same, and every stage | an explicit level: a `float`, and an `int` of a depth texture, which WGSL takes whole |
| `sample(…, bias = b)` | a texture of floats | a bias on the level derivatives pick, in a pixel stage |
| `sample(…, grad_x = …, grad_y = …)` | a texture of floats | the derivatives themselves |
| `gather(…, component = texel_component.y)` | a 2D or cube texture of floats | one channel of the four texels a bilinear sample reads; `.x` by default |
| `sample_compare(…, reference = r)` | a depth texture | a comparison, through a `comparison_sampler`, in a pixel stage |
| `sample_compare(…, reference = r, level = 0.0)` | the same, and every stage | a comparison at level 0, the one level every target compares at |
| `gather_compare(…, reference = r)` | a 2D or cube depth texture | the comparisons of four texels |
| `load(xy, level)` | every texture but a cube | one texel, with no sampler; a multisampled one takes `sample = s` instead |
| `load(xy)`, `store(xy, value)` | an image | one texel of an image the shader may read, or write |
| `img[xy]`, `img[xy] = value` | an image | the same `load` and `store`, as a subscript ([CHK-367](semantics/checking.md#bindings)) |
| `size(level)`, `layer_count()`, `level_count()`, `sample_count()` | textures and images | what the shape has |

An array's layer is always named, `layer = 2`, since it is no coordinate on every target.
An offset, `offset = int2(1, -1)`, is a constant from -8 to 7 on a 2D, 2D array or 3D texture.
A cube and a multisampled texture take none, and neither does a 1D one, which Metal samples with no offset.
A gather's component, an offset and a comparison's level are constants, because some target takes each only as written (CHK-280).

**A texture may name the sampler it is sampled with, and a call then leaves it out.**
`@sampler(name)` on a texture member names a sampler of the same binding, static or dynamic, or a file-scope sampler:

```sgl
binding material:
    @sampler(albedo_smp)
    albedo: texture_2d[float4]
    sampler albedo_smp:
        filter = .linear

// in a pixel stage
let a = material.albedo.sample(uv)
let b = material.albedo.sample(uv, other_smp)
```

A call that names a sampler takes that one, and a call without one on a texture without `@sampler` is `missing-sampler` (CHK-279).
A member of the binding hides a file-scope sampler of its name, and a call through a file-scope one reaches it as naming it would ([EMIT-133](semantics/emitting.md#bindings)).

**A depth texture filters only in a comparison**, because WebGPU refuses a filtering sampler on one otherwise.
So a plain `sample` of a depth texture goes through a `@non_filtering` sampler, as an `@unfilterable` texture does (CHK-281).

## Features

**SGL refuses a non-portable form by feature, never by target.**
A form that some backend lacks is a normal error on every target, unless a `require` of its feature grants it.
Opting in is what makes a shader non-portable on purpose, and the host then asks the device before it builds a pipeline.

```sgl
require extended_image_formats

binding post:
    require readwrite_image_formats
    acc: mut image_2d[.rgba16_float]
```

A `require` stands in the file, in a binding, or in a function body.
It permits a feature and costs nothing where nothing uses it.
What an entry point needs of a device is what the bindings it lists use, which `sgl describe` reports per entry point and per pipeline.
An entry point must declare each of those itself: by its file, by a binding it lists, or in its body.
[Features](semantics/checking.md#features) holds the rules.

| form | the feature that grants it |
|---|---|
| `texture_2d_ms_array` | `sg::feature::multisampled_array_textures`, which WebGPU lacks |
| `mut image*[.F]` with `F` not `r32_float`, `r32_uint` or `r32_sint` | `sg::feature::readwrite_image_formats`, WebGPU's `texture-formats-tier2` |
| `image*[.F]` with `F` outside the portable image formats | `sg::feature::extended_image_formats`, WebGPU's `texture-formats-tier1` |
| filtering a 32-bit float texture | `sg::feature::float32_filtering`, WebGPU's `float32-filterable`; refused by sg at bind time, never by SGL |
| `T[N]` of a resource | `sg::feature::binding_arrays`, which WebGPU lacks ([Binding arrays](#binding-arrays)) |
| `acceleration_structure[.geometry]` | `sg::feature::ray_query`, or `raytracing_pipeline` in a file that grants that one |
| a call that traces inline, `world.trace(r)` | `sg::feature::ray_query`, emulated on WebGPU; counted where the entry point reaches the call (CHK-322) |
| a ray-tracing stage, a trace of a ray type, a callable's call | `sg::feature::raytracing_pipeline`, which WebGPU lacks |
| a value of `half` or of its vectors | `sg::feature::shader_f16`, WebGPU's `shader-f16`; counted where the entry point holds one |
| a value of `short`, `ushort` or their vectors | `sg::feature::shader_int16`, which WebGPU lacks |
| a subgroup operation, `@subgroup_size`, `@subgroup_invocation_id` | `sg::feature::subgroups`, WebGPU's `subgroups` |
| `@coherent` on a member ([Coherent memory](#coherent-memory)) | `sg::feature::device_coherence`, which WebGPU lacks |
| `@atomic` on an image member ([Atomics](#atomics)) | `sg::feature::image_atomics`, which core WebGPU lacks |

`ray_query` and `raytracing_pipeline` are the two halves of ray tracing, and [raytracing.md](raytracing.md) says what each grants.
A device may have either without the other, which is why they are two features.

**`acceleration_structure[.geometry]` is a binding member: the TLAS a trace runs against** (CHK-320).
Its argument is required and names what it holds, `.triangles`, `.procedural` or `.mixed`.
It is `sg::binding_type::acceleration_structure`, a resource like a texture, never a value, and a host binds an `sg::tlas_view` to it.
On WebGPU it takes no binding of its own: sg's acceleration pool and the dispatch's roots stand for it in the reserved group (EMIT-135).

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

## Binding arrays

**`T[N]` of a resource is a binding array**: `N` consecutive slots of one resource type under one name, which a device grants through `require binding_arrays`.

```sgl
require binding_arrays

binding materials:
    @sampler(bilinear) albedo: texture_2d[float4][64]
    params: buffer[float4][8]
    bilinear: sampler

let base = materials.albedo[nonuniform p.material].sample(p.uv)
let fixed = materials.albedo[materials.slot].sample(p.uv)
```

* It is read by element, and an element is the resource itself, handed to a builtin as the member would be.
* It takes `N` slots from its first, so the resources after it start `N` later, as sg's bindings concept requires of every array.
* `N` is at least 2: sg and the generated host code bind a count of 1 as a plain member, which is what one resource is written as (CHK-299).
* An index the uniformity pass cannot prove the same in every invocation is marked `nonuniform i`, or refused (CHK-300).
  Forgetting the mark is silent on the GPU: some hardware reads one invocation's descriptor for its whole wave.
  A mark on an index the pass proves uniform is a warning, since it pays for nothing.
  The mark stands only as the index itself; on a `let` or on a value array's index it would mean nothing, and is refused.
* `T[]`, whose length the host binds, is the spelling an unbounded array has, and `unsupported-yet` until sg binds one.
  An array of samplers, and one of more than one dimension, are `unsupported-yet` too.
* HLSL writes `Texture2D<float4> albedo[64]` and `NonUniformResourceIndex` around a marked index; WebGPU has no binding arrays, so WGSL refuses by the feature.

## Workgroup memory

**A `@workgroup` binding is memory every thread of one workgroup shares**, alive for that workgroup's run: HLSL's `groupshared`, WGSL's `var<workgroup>`, MSL's `threadgroup`.

```sgl
@workgroup binding tile:
    values: float[256]
    total: float

@compute(256) fun reduce(@local_thread_index li: int, @workgroup_id g: int3){work, tile}:
    tile.values[li] = work.input[g.x * 256 + li]
    workgroup_barrier()
    …
```

* It is listed like any binding and takes no group, as `@inline` constants take none, since no host binds it; describe tells the host nothing of it.
* Its members are values — scalars, vectors, structs and arrays of them — and never a resource (CHK-292).
* A shader writes it, and only a compute entry point may list it (CHK-294).
* A barrier waits for every thread of the workgroup, after which each sees what the others wrote (EMIT-131).
  `workgroup_barrier()` makes workgroup memory visible, `storage_barrier()` buffers and `texture_barrier()` images.
* Everything it holds together fits 16 KiB, WebGPU's default limit and vulkan's required minimum (CHK-293).
* Nothing is defined before it is stored: no target but WGSL zeroes it, and a shader that relies on either pays for it on every target.
  The interpreter reports a read of what was never stored as a program error (EVAL-92).
* A test holds workgroup memory of its own run, so it uses a `@workgroup` binding without listing it (CHK-295).
  Every other binding a test reads it lists, `test {frame}:`, and the driver that runs it gives the values (CHK-333).

## Atomics

**`atomic[uint]` and `atomic[int]` are memory every invocation updates in one indivisible step**, and like a resource they are never a value.
One stands as the element of a `mut buffer`, or as a member of a `@workgroup` binding, arrays of them included:

```sgl
binding stats:
    hits: mut buffer[atomic[uint]]

@workgroup binding local:
    count: atomic[uint]

let slot = local.count.add(1)        // the value before the add
stats.hits[slot].max(7)
```

Its operations are builtins that take it first, called as methods: `add`, `subtract`, `min`, `max`, `bit_and`, `bit_or`, `bit_xor` and `exchange` each give the value before,
`load()` reads it, and `store(v)` writes it.
`and` and `or` are keywords, so the bitwise updates carry the `bit_` their operators lack.
Every one is relaxed, the one ordering WGSL has, and a vertex stage has none, since WebGPU has no writable storage there (CHK-296).
The host holds a buffer of atomics as the plain integers it is.
Floats, 64 bits, a compare-exchange and a vertex stage are [the incubator's](incubator/atomics.md).

**An image's texels are atomics where its member says so.**
`@atomic` on a `mut` image of the format `r32_uint` or `r32_sint` makes each texel an atomic, which a subscript reaches and the methods above update ([CHK-372](semantics/checking.md#atomics)):

```sgl sketch
require image_atomics

binding prepare:
    @atomic depth: mut image_2d[.r32_uint]

prepare.depth[xy].max(d.bits)
```

Atomic is a property of the place, as it is for a buffer, so a plain `load` or `store` of such an image is refused and never races an update.
An image used atomically in one pass and plainly in another is two members, or two bindings of one view.
It needs `image_atomics`, which core WebGPU lacks.

## Coherent memory

**`@coherent` on a `mut` buffer or a `mut` image makes what one workgroup writes visible to another within the same dispatch** ([CHK-368](semantics/checking.md#bindings)).
A write is published by a barrier of its workgroup, `storage_barrier` for a buffer and `texture_barrier` for an image, and an atomic update after that barrier tells the readers it is there.
That is the shape of a single-pass reduction whose last workgroup reads what every other one wrote:

```sgl sketch
require device_coherence

binding spd:
    @coherent mip5: mut image_2d[.rgba16_float]
    @coherent counter: mut buffer[atomic[uint]]
```

Without it a GPU whose per-core caches are not kept coherent may hand the reader a stale line.
It costs every access to the member, and it needs `device_coherence`, which WebGPU lacks: there such a handoff is two dispatches.

## How a group reaches sg

**The slots of a group are its members in declaration order**, after the constant block at slot 0 when the group has plain members.
A group-scope static sampler takes a slot like any sampler, since sg matches it by name to a sampler binding.

**SGL states every fact of a binding itself.**
Dimension, sample type, image format, access and sampler kind are in the declaration, and `sgl describe` hands each to the host.
The generated group's table is what the host builds its layout from, so that is where every fact reaches sg.
The compiled shader states them too, from the same declaration: slib builds it from what SGL says, and takes only the bytecode from the target's compiler.
That compiler's reflection of the text is compared on every compile, and a disagreement is logged as the SGL bug it is, never used.
The WGSL SGL writes states every fact; HLSL states the dimension, and the vulkan text an image's format as `[[vk::image_format]]`.
HLSL cannot say `unfilterable` at all, which costs nothing, since dx12 and vulkan read none of it.
Building the layout by reflecting the WGSL instead was declined: that text is written from the same declaration, so reading it back is `sgl describe` with a parser in between.
And `texture_2d<f32>` fits a filterable and an unfilterable layout alike, so the reflection could not even recover `@unfilterable`.

**MSL.**
sg's metal backend makes a group one argument buffer at `[[buffer(group)]]`, whose member `[[id(n)]]` is slot `n` of the group.
A texture or sampler slot holds a resource id, so a group reads in MSL as (EMIT-89):

```cpp
struct post_arguments
{
    constant post_data* post [[id(0)]];
    texture2d<float> post_src [[id(1)]];
    texture2d<float, access::write> post_dst [[id(2)]];
    sampler post_bilinear [[id(3)]];
};

kernel void blur(uint3 id_in [[thread_position_in_grid]], constant post_arguments& post_group [[buffer(0)]])
{
    constant auto& post = *post_group.post;
    constant auto& post_src = post_group.post_src;
    ...
```

Every call is inlined, so resources are parameters of the entry point alone, and the locals at its top give the body the names the other targets' globals have.
An image's access maps one-to-one onto `access::read`, `access::write` and `access::read_write`, and a depth texture is `depth2d<float>`.
A group's static sampler is a slot like any other, which the backend fills from the layout.
A file-scope static sampler is a `[[sampler(i)]]` parameter of the entry point, filled from the pipeline layout ([EMIT-133](semantics/emitting.md#bindings)).

## Footprint

**An entry point carries its footprint: what its code does to each slot of the bindings it lists.**
A binding's access is what its layout permits, and a group is declared once and bound to many pipelines, so that is the union of what they all do.
One entry point usually does less: a `mut buffer` it only loads from is a read to it, and a member it never names is untouched.
sg places barriers by the footprint, so that difference is a barrier a pipeline does not pay.

**A slot is one resource member, or a binding's constant block as a whole.**
The plain members of a group share one constant buffer, so reading any of them reads the block, spelled by the binding's name alone.
A sampler is no slot: nothing orders against one.
An `@inline` binding is none either, since sg sets it as constants rather than binding it.

**How a slot is touched: read, write, or both.**

* A buffer element read as a value is a read, and one assigned to is a write; `values[i] += x` is both.
* A resource handed to a builtin is used as the builtin's parameter declares it: an `out` parameter writes, a `mut` one reads and writes, and an unmarked one reads.
  `size` reads, since the text it becomes uses the resource.
* A constant member read anywhere reads the block.
* An image subscript is the `load` or `store` it stands for, and an update of an `@atomic` texel reads and writes.

**It is computed over the code the entry point runs, after inlining, in the form an emitter prints.**
So a use in a called function counts, a use in a branch that may not run counts, and a use only a check or an `assert` makes does not, since the text holds neither.
A use in a branch on a constant that is false is no use at all: the language removes that branch from the text, and so from the footprint ([CHK-356](semantics/checking.md#the-flat-tree)).
That is what lets an option leave a binding untouched, rather than a choice of the target compiler's dead-code elimination.
That is the invariant sg relies on: **the footprint covers everything the emitted text uses**, because a slot it calls untouched gets no layout transition at all.
It is its own pass over that tree rather than something each emitter records as it prints.
One pass serves every target and a pin judges it once, where four emitters would each have to tell a load from a store at every print site.

**`sgl describe` reports it**, one `slot: access` per touched slot, and slib hands it to sg with the compiled shader.
A pin in a source states it, which [CHK-267](semantics/checking.md#entry-points) judges:

```sgl
@expect(footprint = "work: read, work.values: read write")
@compute(64) fun scale(@thread_id id: int3){work}:
    work.values[id.x] += work.factor
```

## What the compiler carries today

The syntax above is what the AST builds; the check pass is what limits it.
Everything not named here is the diagnostic `unsupported-yet`, never a guess.

* `buffer[T]` and `mut buffer[T]`, for a `T` that is a scalar or a vector.
* A subscript on a buffer, as a value and as the place of an assignment.
* A subscript on an image, as its `load` and as the place its `store` writes ([CHK-367](semantics/checking.md#bindings)); a texture's is not built.
* Every texture, depth texture, image and sampler form above, with `@unfilterable` and `@non_filtering`.
* A static sampler in a binding, and the `needs-feature` refusals.
* A file-scope sampler, handed to a builtin by its name ([CHK-314](semantics/checking.md#bindings)).
* A texture, an image or a sampler handed to a builtin, which is the only way one is used ([CHK-206](semantics/checking.md#bindings)).
  The builtins that take one are the texture methods of [Sampling](#sampling), called as methods of it: `tex.sample(uv, smp)`.
* A plain member of a group, as a field of the constant buffer the group owns, for a type whose place in a block every target agrees on.
* The positional group numbering, and `@inline` last.
* Binding arrays of textures, images and buffers, under `require binding_arrays`, with `nonuniform`.
* `@workgroup` bindings, which take no group.
* `atomic[uint]` and `atomic[int]`, in a `mut buffer` and in workgroup memory, with every update but a compare-exchange.
* `@atomic` images of `.r32_uint` and `.r32_sint`, with the same updates, and `@coherent` buffers and images.
* `acceleration_structure[.geometry]`, which the prelude's `trace` takes ([raytracing.md](raytracing.md)).
* A resource's host name, its path `binding.member` ([CHK-171](semantics/checking.md#bindings)), which the text reports beside the identifier it minted.

Every target writes a group.
WGSL gives each resource its own `@group`/`@binding`, and HLSL declares each at file scope with `register(<class>slot, spaceN)` on dx12 and `[[vk::binding(slot, N)]]` on vulkan.
MSL writes the group as one argument buffer whose member `[[id(slot)]]` is each slot, as [MSL](#how-a-group-reaches-sg) below shows and EMIT-89 states.
A group's plain members are one constant buffer at the group's slot 0, named after the binding, and its resources follow it in declaration order.
A file-scope sampler stands where sg binds a pipeline layout's static sampler of its index, i ([EMIT-133](semantics/emitting.md#bindings)):
`register(s<i>, space10)` on dx12, `[[vk::binding(i + 1, 3)]]` on vulkan, `@group(3) @binding(i + 1)` in WGSL, and a `[[sampler(i)]]` parameter in MSL.

`bytes` and `constants[T]` both parse and are then reported.
A struct element type is placed by the storage rule of [the layout rules](semantics/emitting.md#layout).
That is deliberate.
The shape is decided, so it is written down here and the AST constructs it.
A shader using one then gets a diagnostic that names the feature, rather than a parse error that names nothing.

## Open

* `values.length` — WGSL, HLSL and SPIR-V can all answer it without packing data, and MSL cannot: a Metal buffer argument is a pointer and carries no length.
  Until metal is a target with a compiler behind it, a shader passes the count in.
* A buffer as a field of a struct, which is `unsupported-yet` like every buffer outside a binding member.
  It could be allowed where the buffer is hoisted and stays uniform across every use, a scalarization of the struct that inlining makes possible.
* The functions over textures and images — sampling, loads, stores and sizes — and a default sampler on a texture member ([texture-methods.md](incubator/texture-methods.md)).
* Arrays of textures and images, which `require binding_arrays` grants once they exist.
  Their footprint says whether and how the code indexes them and whether an index is dynamic; which elements it reaches stays the host's to declare per dispatch.
* SGL may come to name that per-dispatch array declaration itself — tentatively `access` — rather than leaving it to the host alone.
* Whether a footprint entry distinguishes a uniform dynamic index from a non-uniform one, which some backends pay for differently.
