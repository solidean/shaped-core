# SGL Bindings

A **binding group** is what a shader needs from outside: the buffers, textures and samplers a host binds before a dispatch or a draw.
This file is the model — what a group is, what may stand in one, and how it reaches a target.
The rules live where every other rule does.
[AST-73 to AST-76](syntax/ast.md#bindings-and-samplers) is the syntax.
[CHK-40 to CHK-46](semantics/checking.md#bindings) is what a group means.
[EMIT-82 onward](semantics/emitting.md#bindings) is what a target sees.
Back to the [specification](_index.md).

## A group and its members

```sgl
binding work:
    scale: float
    src: buffer[float]
    dst: mut buffer[float]
```

A member is a name and a type, like a struct field.
What kind of resource it is follows from that type, and nothing else marks it.

**A member of a plain value type is a constant**, and the group's plain members together are one constant buffer the compiler builds.
So `scale: float` above costs no declaration of its own: the group has an implicit constant buffer, and `scale` is a field of it.
That is the common case, and it is why `constants[T]` below is rarer than it looks.

**A member of a resource type is that resource.**
The table is the one sg already commits to.
`sg::binding_type` in [binding.hh](../../../shaped-graphics/src/shaped-graphics/binding/binding.hh) is the portable set every backend maps to, and SGL spells it rather than deciding it again.

| SGL | `sg::binding_type` | what it is |
|---|---|---|
| a plain value type | `uniform_buffer` | a field of the group's implicit constant buffer |
| `constants[T]` | `uniform_buffer` | a constant buffer that comes from somewhere else, already laid out |
| `buffer[T]` | `readonly_structured_buffer` | an array of `T` the shader reads |
| `mut buffer[T]` | `readwrite_structured_buffer` | an array of `T` the shader reads and writes |
| `bytes` | `readonly_raw_buffer` | raw bytes, addressed by offset |
| `mut bytes` | `readwrite_raw_buffer` | raw bytes the shader also writes |
| `texture2d[T]` and its neighbours | `readonly_texture` | a texture the shader samples |
| `mut texture2d[F]` | `readwrite_texture` | a storage texture the shader reads and writes |
| `out texture2d[F]` | `readwrite_texture` | a storage texture the shader only writes |

## Access

Three spellings, and a resource takes the ones it has.

* Unmarked — the shader only reads it.
* `mut` — the shader reads and writes it.
* `out` — the shader only writes it.

**A buffer is never `out`.**
No target has a write-only buffer, and WGSL refuses one in as many words: *access mode 'write' is not valid for the 'storage' address space*.
A storage texture has all three, and the third is not a convenience.
Core WebGPU allows read-write storage access only for the `r32` formats, so a storage texture of any other format has to be `out` there.
dx12 and vulkan do not ask — a UAV is read-write to them whatever the shader said.

This asymmetry is WebGPU's rather than ours, and sg already carries it as `sg::storage_access`.

## A storage texture carries its format

`mut texture2d[rgba8unorm]` names a format where `texture2d[float4]` names a component type.
WGSL is why: it spells a storage texture `texture_storage_2d<rgba8unorm, write>`, with the format inside the type, and a shader that left it out could not be written for WebGPU at all.
HLSL needs only the component type and ignores the rest, so carrying the format costs the other targets nothing.

A format that core WebGPU does not allow for the access it is asked for is a normal error, and its message names the feature to opt into.

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

## What the compiler carries today

The syntax above is what the AST builds; the check pass is what limits it.
Everything not named here is the diagnostic `unsupported-yet`, never a guess.

* `buffer[T]` and `mut buffer[T]`, for a `T` that is a scalar or a vector.
* A subscript on a buffer, as a value and as the place of an assignment.
* A plain member of a group, as a field of the constant buffer the group owns, for a type whose place in a block every target agrees on.
* The positional group numbering, and `@inline` last.
* A buffer's host name, its path `binding.member` ([CHK-171](semantics/checking.md#bindings)), which the text reports beside the identifier it minted.

Three targets write a group, and the fourth declines rather than guessing.
WGSL gives each resource its own `@group`/`@binding`, and HLSL writes `#pragma sc group n` and a namespace, so that every register stays slib's binding pass's to assign.
A group's plain members are one constant buffer at the group's slot 0, named after the binding, and its buffers follow it.
MSL takes a buffer as an argument of the entry point rather than as a global, which this writer does not build yet, so it reports `unsupported`.

A struct element type, `bytes`, `constants[T]`, every texture form and a `sampler` member all parse and are then reported.
That is deliberate.
The shape is decided, so it is written down here and the AST constructs it.
A shader using one then gets a diagnostic that names the feature, rather than a parse error that names nothing.

## Open

* `values.length` — WGSL, HLSL and SPIR-V can all answer it without packing data, and MSL cannot: a Metal buffer argument is a pointer and carries no length.
  Until metal is a target with a compiler behind it, a shader passes the count in.
* A `sampler` member collides with the `sampler` keyword, which declares a static sampler.
  A binding member `smp: sampler` reads as a keyword form today, so the dynamic-sampler member has no spelling yet.
* The layout of a struct element type: our own rules, portable across the four targets, with explicit padding where they need it, and the generated host struct matching byte for byte.
