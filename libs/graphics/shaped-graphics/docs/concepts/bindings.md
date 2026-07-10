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

A backend-specific binding vocabulary would be HLSL/D3D12 verbatim — a bind-type enum that *is*
`D3D_SHADER_INPUT_TYPE`, an address model that is HLSL's `(space, register, count)`. Since sg's baseline
shading language is undecided, the binding vocabulary is instead drawn from concepts common to
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
[`sampler`](../../src/shaped-graphics/sampler.hh) — an immutable filtering/addressing/LOD
state — via a `named_sampler`. There are two ways in, and *which one* is a layout-time decision:

- **static** — fixed for every group and costs no per-group descriptor. Two ways to declare one, and you
  can use either or both: a **name-matched** `named_sampler` passed to `create_binding_group_layout`
  (matched to a sampler binding by name, then excluded from the dynamic group), or a **register-bound**
  `bound_sampler` attached to the `pipeline_layout` directly (its `binding` carries the register/space, so
  it needs no matching group binding — e.g. a shared sampler a pipeline needs on top of its groups). A
  sampler binding declared static this way must not be supplied per group. (dx12: both become
  `D3D12_STATIC_SAMPLER_DESC`s that the pipeline layout bakes into the root signature.)
- **dynamic** — a sampler binding *not* named static is supplied per group: each `binding_group` provides
  its `named_sampler`, so the sampler state can vary group to group. (dx12: samplers occupy their own
  descriptor heap and root descriptor table, so a group with dynamic samplers binds a second heap and
  table at dispatch — see the dx12 section below.)

## Where this is headed

Bindings are the input to the rest of the descriptor system, which consumes them (and the `raw_view`s
bound to them) to reach the GPU:

```
compiled_shader.bindings ─▶ binding_group_layout ─▶ binding_group (name → raw_view, validated) ─▶ command_list.bind + dispatch
   (reflection)              (one group's schema)     (the first raw_view consumer; backend → native descriptor)
                                     └─▶ pipeline_layout (ordered group layouts) ─▶ compute_pipeline
```

A `pipeline_layout` composes an ordered list of `binding_group_layout`s (index = bind slot), so an entire
group can be rebound at one slot without disturbing the others. It may also carry an optional
**inline-constants** block — a single uniform-buffer binding, excluded from the group layouts, written
directly on the command list via `cmd.compute.set_inline_constants(...)` for fast per-dispatch parameters
without descriptor allocation (dx12 root constants / vulkan push constants). `binding_group_layout`, `pipeline_layout`,
and `compute_pipeline` are schemas / PSOs, not lifetime-scoped resources — they are built through the raw
[`ctx.uncached.create_*`](../../src/shaped-graphics/context.uncached.hh) scope, or (almost always preferred)
deduplicated and built asynchronously through
[`ctx.cached.acquire_*`](../../src/shaped-graphics/context.cached.hh). See [caches](caches.md).
A `binding_group`, being a per-instance set of bound resources, is genuinely lifetime-scoped:
`ctx.persistent.create_binding_group` for one that lives until released, `ctx.transient.create_binding_group`
for per-frame scratch recycled when its epoch retires. The `command_list` recording that binds and
dispatches them (`cmd.compute.bind_pipeline` / `bind_group` / `dispatch`) is lifetime-agnostic.

The **dx12** backend implements the full chain — a `pipeline_layout` becomes the root signature (composed
from its group layouts' descriptor tables + baked static samplers, plus a trailing 32-bit-constants root
parameter when the layout declares inline constants), a `binding_group` allocates a range in
the single shader-visible descriptor heap and translates each `raw_view` into a native CBV/SRV/UAV, and a
dispatch binds each slot's table and runs. That heap is **split by lifetime**, and the two halves use
different allocators because their hazard models differ:

- a leading **transient ring** reclaimed per epoch. Descriptors are **written by the CPU** when a group
  is created and read by the GPU during that epoch, so a slot can't be reused until the epoch that wrote
  it retires — a CPU/GPU in-flight hazard the ring's per-epoch watermark enforces. (This is unlike the
  transient *buffer* heap, whose contents are only GPU-touched, so it can bump-reset and alias across
  epochs — see [memory](memory.md).) A transient group bound past its epoch is refused (its slots may
  already be reused).
- a **persistent free-ranges allocator** for the rest: a group's range is returned to the free list when
  the group is released, deferred (via an epoch finalizer, like buffer deletion) until its last-using
  epoch retires — so long-lived groups don't leak the heap.

The **vulkan** backend stubs the chain (each create returns a `cc::error`) until its own compute milestone.
Compilation is still external: a `compiled_shader`'s bytecode + reflection are supplied (the dx12 test
embeds a precompiled DXIL blob).

## Deferred

Constant-buffer member layouts, a content hash for caching, and input/output (vertex / render-target)
signatures are all future additions to `compiled_shader`; the binding vocabulary still gains an
acceleration-structure kind. Inline (root/push) constants exist on the `pipeline_layout` for compute;
per-member payload validation (a reflected constant-buffer layout) and the graphics / raytracing scopes
are deferred. (The DXC shader compiler reflects buffer / texture / sampler
bindings; texel/typed buffers and acceleration structures are the remaining unsupported kinds.)

## See also

- [binding.hh](../../src/shaped-graphics/binding.hh) — `binding`, `binding_type`, `access_of` / `shape_of` / `accepts`.
- [compiled_shader.hh](../../src/shaped-graphics/compiled_shader.hh) — the shader data model.
- [views](views.md) — the bound half: `raw_view` and the typed views that convert to it.
