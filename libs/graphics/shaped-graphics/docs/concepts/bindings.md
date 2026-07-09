# Concept: bindings & compiled shaders

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [views](views.md).

A [`compiled_shader`](../../src/shaped-graphics/compiled_shader.hh) is a bytecode blob plus the
metadata and **reflection** needed to build a pipeline and bind resources to it. Its reflection is a
flat list of [`binding`](../../src/shaped-graphics/binding.hh)s — the named resource slots the shader
declares. This is the schema half of the descriptor system; the bound half is [views](views.md).
(Compilation itself is not part of sg yet — a `compiled_shader` is produced by a future compiler or
loader, or hand-authored in a test.)

## The binding vocabulary is backend-agnostic

The legacy GFX system's binding vocabulary was HLSL/D3D12 verbatim — its bind-type enum *was*
`D3D_SHADER_INPUT_TYPE`, its address model was HLSL's `(space, register, count)`. Since sg's baseline
shading language is undecided, the binding vocabulary is drawn from concepts common to
HLSL / GLSL / Slang / MSL / WGSL:

- **`binding_type`** — the kind of resource a slot expects (`uniform_buffer`,
  `readonly_structured_buffer`, `readwrite_structured_buffer`, `readonly_raw_buffer`,
  `readwrite_raw_buffer`, `readonly_texture`, `readwrite_texture`, `sampler`; acceleration-structure
  follows). The backend-agnostic replacement for `D3D_SHADER_INPUT_TYPE`.
- **`(set, index)` + `count`** — the address, following SPIR-V (set/binding), WGSL (`@group`/`@binding`),
  and Metal argument buffers. A D3D12 backend derives its `(register-type, register, space)` at layout
  build (register-type from `binding_type` → `t`/`u`/`b`/`s`, register = `index`, space = `set`).
  `count == 0` is an unbounded array.
- **`block_size`** — a uniform block's declared byte size, used to validate a bound view's size.

## Bindings and views speak the same vocabulary

A `binding` describes what the shader *expects*; a [`raw_view`](../../src/shaped-graphics/views.hh)
describes what is *bound*. For buffer and texture kinds they line up exactly: `access_of(binding_type)` and
`shape_of(binding_type)` give the `(view_class, view_shape)` a satisfying view must have, and
`accepts(binding_type, raw_view)` is the check. That equivalence is what lets a binding validate a
bound view without a backend — and it is why `binding_type`'s view kinds mirror the view
`(access, shape)` combinations one-to-one.

## Samplers: not views

A `sampler` binding (`is_sampler(binding_type)`) has no view — a sampler carries no memory and no
`(access, shape)`, so `accepts` rejects any view for it. It is matched instead to a
[`sampler_description`](../../src/shaped-graphics/sampler.hh) — an immutable filtering/addressing/LOD
state — via a `named_sampler`. There are two ways in, and *which one* is a layout-time decision:

- **static** — a `named_sampler` passed to `create_binding_layout` is baked into the layout's native
  root signature (D3D12 `D3D12_STATIC_SAMPLER_DESC`); it is fixed for every group and consumes no heap
  slot. A sampler binding named in the layout's `static_samplers` must not be supplied per group.
- **dynamic** — a sampler binding *not* named static is a table entry; each `binding_group` supplies its
  `named_sampler`, written into a separate sampler descriptor heap. D3D12 keeps samplers in their own
  heap and their own root descriptor table (one of each type bound at a time), so a group with dynamic
  samplers binds two tables at dispatch.

## Where this is headed

Bindings are the input to the rest of the descriptor system, which consumes them (and the `raw_view`s
bound to them) to reach the GPU:

```
compiled_shader.bindings ─▶ binding_layout ─▶ binding_group (name → raw_view, validated) ─▶ command_list.bind + dispatch
   (reflection)              (the schema)       (the first raw_view consumer; backend → native descriptor)
```

`binding_layout` and `compute_pipeline` are cached schemas — always `ctx.persistent.create_*`.
A `binding_group`, being a per-instance set of bound resources, comes in both lifetimes:
`ctx.persistent.create_binding_group` for one that lives until released, `ctx.transient.create_binding_group`
for per-frame scratch recycled when its epoch retires. The `command_list` recording that binds and
dispatches them (`cmd.compute.bind_pipeline` / `bind_group` / `dispatch`) is lifetime-agnostic.

The **dx12** backend implements the full chain — a `binding_layout` becomes a root signature, a
`binding_group` allocates a range in the single shader-visible descriptor heap and translates each
`raw_view` into a native CBV/SRV/UAV, and a dispatch binds the table and runs. That heap is **split by
lifetime**, and the two halves use different allocators because their hazard models differ:

- a leading **transient ring** reclaimed per epoch. Descriptors are **written by the CPU** when a group
  is created and read by the GPU during that epoch, so a slot can't be reused until the epoch that wrote
  it retires — a CPU/GPU in-flight hazard the ring's per-epoch watermark enforces. (This is unlike the
  transient *buffer* heap, whose contents are only GPU-touched, so it can bump-reset and alias across
  epochs — see [memory](memory.md).) A transient group bound past its epoch is refused (its slots may
  already be reused).
- a **persistent free-ranges allocator** for the rest: a group's range is returned to the free list when
  the group is released, deferred (via an epoch finalizer, like buffer deletion) until its last-using
  epoch retires — so long-lived groups don't leak the heap.

The **vulkan** backend stubs the chain (`CC_UNREACHABLE`) until its own compute milestone. Compilation is
still external: a `compiled_shader`'s bytecode + reflection are supplied (the dx12 test embeds a
precompiled DXIL blob).

## Deferred

Constant-buffer member layouts, root/push constants, a content hash for caching, and input/output
(vertex / render-target) signatures are all future additions to `compiled_shader`; the binding vocabulary
still gains an acceleration-structure kind. (The DXC shader compiler reflects buffer / texture / sampler
bindings; texel/typed buffers and acceleration structures are the remaining unsupported kinds.)

## See also

- [binding.hh](../../src/shaped-graphics/binding.hh) — `binding`, `binding_type`, `access_of` / `shape_of` / `accepts`.
- [compiled_shader.hh](../../src/shaped-graphics/compiled_shader.hh) — the shader data model.
- [views](views.md) — the bound half: `raw_view` and the typed views that convert to it.
