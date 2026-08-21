# Concept: bindings & compiled shaders

A [`compiled_shader`](../../src/shaped-graphics/binding/compiled_shader.hh) is a bytecode blob plus the metadata and **reflection** needed to build a pipeline and bind resources to it.
Its reflection is a flat list of [`binding`](../../src/shaped-graphics/binding/binding.hh)s — the named resource slots the shader declares.
This is the schema half of the descriptor system; the bound half is [views](views.md).
Producing a `compiled_shader` is not sg's job — it comes from shaped-shader-library (see [shaders](../shaders.md)), or is hand-authored in a test.

## The binding vocabulary is backend-agnostic

A backend-specific binding vocabulary would be HLSL/D3D12 verbatim: a bind-type enum that *is* `D3D_SHADER_INPUT_TYPE`, an address model that is HLSL's `(space, register, count)`.
sg's baseline shading language is undecided, so the vocabulary is drawn instead from concepts common to HLSL / GLSL / Slang / MSL / WGSL:

- **`binding_type`** — the kind of resource a slot expects, and the backend-agnostic replacement for `D3D_SHADER_INPUT_TYPE`.
  `uniform_buffer`, `readonly_structured_buffer`, `readwrite_structured_buffer`, `readonly_raw_buffer`, `readwrite_raw_buffer`.
  Then `readonly_texture`, `readwrite_texture`, `sampler`, `acceleration_structure`.
- **`index` + `count`** — the address within the group, following SPIR-V (`binding`), WGSL (`@binding`) and Metal argument buffers.
  A D3D12 backend derives its `(register-type, register)` at layout build: register-type from `binding_type` → `t`/`u`/`b`/`s`, register = `index`.
  `count == 0` is an unbounded array, which sg rejects — see [Array bindings](#array-bindings) for why.
- **`group_index`** and **`space`** — the two ways a shading language namespaces that address, each optional and each reflected by the languages that have it.
  They are kept apart because only one of them is hardware-visible; the section below is what that costs a caller.
- **`block_size`** — a uniform block's declared byte size, used to validate a bound view's size.

## A group index binds, a space only numbers

A **group index** is a descriptor set the hardware sees: SPIR-V's `set`, WGSL's `@group`.
Vulkan guarantees four of them, and binding a descriptor set means naming the very index the shader was compiled against — bind it elsewhere and the shader reads the wrong table.
A **space** is HLSL's `space`, and it is only a namespace for register numbers: `t0, space1` and `t0, space2` are two distinct registers.
Neither of them says anything about which descriptor table they end up in.
An arbitrary number of spaces is fine, which is exactly why a space could never stand in for a set.

So a `binding` carries whichever its language reflects, and neither is invented for it:

- **DXC reflection fills `space`** — always, even for the default space 0 — and leaves `group_index` absent.
- **SPIR-V reflection fills `group_index`**, as would any other language where the set is part of the binding.
- **A hand-written binding fills what it means.**
  Absent `space` means "this shading language has no register spaces", which is not the same as space 0.
  The structural layout hash writes a presence byte, so the two produce distinct `binding_group_layout` objects even with byte-identical root signatures, and `bind_group` compares layouts by pointer.
  HLSL always has a space, so the dx12 backend requires one and asserts on absence; a hand-written binding for it says `.space = 0` explicitly.
  Absent `group_index` means the bind slot alone decides.

A declared group index then propagates, so that it cannot be declared and quietly ignored:

- **A `binding_group_layout` inherits it from its bindings.**
  All the bindings that declare one must declare the same one — they end up in a single group, and a group is bound at a single slot.
- **A `binding_group` reaches it through its layout**, which the backend already holds to match the bind slot's schema.
- **Every backend's `bind_group` asserts the slot matches**, next to the assert that the layout matches the pipeline layout's slot at all.
  A layout that pins no group index binds anywhere, which is the HLSL path and stays free.

## Bindings and views speak the same vocabulary

A `binding` describes what the shader *expects*, and a [`raw_view`](../../src/shaped-graphics/resource/views.hh) describes what is *bound*.
For buffer and texture kinds they line up exactly: `access_of(binding_type)` and `shape_of(binding_type)` give the `(view_class, view_shape)` a satisfying view must have.
`accepts(binding_type, raw_view)` is the check.
That equivalence is what lets a binding validate a bound view with no backend involved, and it is why `binding_type`'s view kinds mirror the view `(access, shape)` combinations one-to-one.

## Array bindings

An array binding is a `binding` with `count > 1`: `count` consecutive descriptors under one name — `Texture2D Texs[4]` in HLSL, and the building block of a bindless table.
`count == 0` (unbounded) is **rejected**: `try_create_binding_group_layout` returns an error naming the binding, on every backend alike.
A bindless table declares a bounded count and treats it as capacity.

The reason is WebGPU, which has no unbounded binding arrays, and sg is meant to stay ready for the day it does.
So this is the portable floor the storage-buffer offset rules in [views.md](views.md) already use: fail loudly on the dx12 dev box rather than letting it surface later on the weaker backend.
The shape stays conditional enough that full support later needs no redesign.
It is an error rather than an assert because these bindings usually come from reflecting someone's shader, which makes an unbounded array content rather than a contract violation.
See [error-handling.md](../../../../../docs/error-handling.md).
The dx12 layout builder keeps its own assert as a backstop.

Three rules distinguish an array binding from a scalar one:

- **A `named_view` supplies exactly `count` views, one per element.**
  A vacant element is the **`sg::vacant_view` marker** — no view at all.
  The backend synthesizes its **null descriptor** from the *binding* alone: access and shape from `binding_type`, a texture's dimension from `binding.texture_dimension`.
  Reflection fills `texture_dimension`; hand-written bindings must set it.
  Reads of a null descriptor return zero.
  All-vacant is legal — a table can start empty — and a null-handle view is an error everywhere: a view always binds a resource.
- **Access is never inferred.**
  Which elements a shader indexes, and how, cannot be read from the binding, so array bindings skip the automatic hazard tracking scalar bindings get.
  The dispatching caller declares the touched elements via `cmd.compute.declare_array_buffer_access` / `declare_array_texture_access`, applied to the next dispatch only.
  The raytracing scope has the same pair.
- **Every bound array binding must be declared before each dispatch** — the backend asserts it.
  A missing declaration is a bug, never "no access"; an empty element span is the way to say "unused this dispatch".

Element resources are still kept alive by the group, exactly like scalar bindings.
Arrays of samplers, uniform buffers or acceleration structures are not supported.
Raster draws do not support array bindings yet.

## Staging a group instead of rebuilding it

A `binding_group` is immutable, so changing one binding means creating the group again — and creating it again writes *every* descriptor.
At a handful of bindings that is free.
At a bindless table of a few thousand it is the whole cost: re-pointing one texture reissues thousands of descriptor writes.

[`staging_binding_group`](../../src/shaped-graphics/binding/staging_binding_group.hh) is the mutable builder that breaks that coupling.
It owns a CPU-side descriptor image, `set` updates one descriptor in it, and `snapshot()` mints an immutable `binding_group` from the whole image.
The immutable groups downstream are unchanged: a snapshot is an ordinary persistent `binding_group`, bound and hazard-declared like any other.

- **A binding is addressed by `binding_slot`**, which `slot_of(name)` resolves once.
  The slot is not a descriptor position: it indexes an internal table built at construction, pairing the layout's own `binding` with the heap and index its descriptors start at.
  That indirection is what every set resolves and bounds-checks against, and it is why setting costs no string lookup and no search.
  Validation reads the layout's binding through it rather than a copy, so what a set is checked against is exactly what the layout declared.
- **The setters name the shape they act on and never infer it.**
  `set_binding` is for a scalar binding and rejects an array.
  `set_array_element` / `set_array_range` / `set_array` and their `unset_` twins are for an array binding and reject a scalar.
  An element index is always an argument, never chosen for you, and always checked against the binding's count.
  There is no `unset_binding`: only an array element can be *absent*, while a scalar is bound for the group's life and merely set to another view.
  An empty scalar is one of those views rather than an absence — `sg::tlas_view{}` is the null acceleration structure every ray misses.
- **`set_array` replaces, `set_array_range` patches.**
  Both take a run of views and may place it at an offset, but `set_array` clears every element the run does not cover, so the array afterwards holds exactly what was passed.
  `set_array_range` leaves everything outside the run alone.
- **A snapshot is cached while nothing has changed.**
  Two snapshots with no `set` between them are the *same* handle, so an unchanged frame copies nothing and rebinds nothing.
- **A staging group starts fully vacant**, every descriptor at its binding's empty value.
  Every binding still has to be set at least once before the first snapshot, exactly as `create_binding_group` rejects a binding that was not provided.
  The demand is that you say what a binding holds, not that it holds anything: an array element may stay vacant, and `unset_array` — or even an empty range — is a perfectly good answer.
  What it catches is the binding nobody wired, which reads zero at runtime and looks like a shader bug.
  Only a static sampler is exempt, having no descriptor in the group to set.
- **Snapshots are independent of the builder and of each other.**
  Mutating after a snapshot only dirties the cache: each snapshot keeps its own resources alive, and stays valid for as long as anyone holds its handle.
- **Not thread-safe** — one owner mutates it, and `snapshot()` counts as a mutation.

In **dx12** the staging image is a private **non-shader-visible** descriptor heap, one per staging group.
A `set` is a single `Create*View` into it.
The backend seam is per *run* rather than per descriptor: one call writes a whole range, and a second clears one.
So setting a range of a thousand elements is one dispatch, and a full `set_array` is three at most.
A dirty `snapshot` allocates a persistent range in the shader-visible heap and fills it with one `CopyDescriptorsSimple`.
That single driver-side descriptor copy, in place of a `Create*View` per slot, is what makes a large table affordable to re-snapshot.
A clean `snapshot` does nothing at all.

## Filling a bindless table: bindless_array

A staging group makes a big table cheap to *change*; what a bindless renderer still needs is the mapping from a view to the element index its shader indexes with.
[`bindless_array`](../../src/shaped-graphics/binding/bindless_array.hh) is that mapping, over exactly one array binding: `bindless_array::for_binding(ctx, group, "Textures")`.

It is deliberately small: it shares the group's handle, so the group cannot go out from under it, but it owns no descriptor and mints nothing.
The layout, how many tables there are and what they are called all stay with the caller.
One array touches nothing but its own binding, so several arrays over one group are independent.

- **`acquire(view)` returns the element index**, minting one on a miss and writing exactly one staging descriptor.
  Identity is the view's hash, so re-acquiring the same view is O(1), returns the same index and touches no descriptor.
  An unchanged working set therefore never dirties the group, and its snapshot is the cached one.
- **An index is valid only for the epoch it was acquired in.**
  Re-acquire the working set every epoch.
  When the array is full, every index not acquired this epoch is reclaimed at once — the mint dirties the group and forces a snapshot anyway, so there is nothing to save by evicting less.
  If every index was acquired this epoch, the working set exceeds the binding's count and `acquire` asserts.
- **`lock()` refuses acquires until `unlock()`**, in the same epoch, and mints nothing.
  It guards the window in which a snapshot is bound; taking that snapshot stays the group owner's job.

## Samplers: not views

A `sampler` binding (`is_sampler(binding_type)`) has no view: a sampler carries no memory and no `(access, shape)`, so `accepts` rejects any view for it.
It is matched instead to a [`sampler`](../../src/shaped-graphics/binding/sampler.hh) — an immutable filtering/addressing/LOD state — via a `named_sampler`.
There are two ways in, and *which one* is a layout-time decision:

- **static** — fixed for every group, and costing no per-group descriptor.
  Two ways to declare one, usable either or both.
  A **name-matched** `named_sampler` passed to `create_binding_group_layout`, matched to a sampler binding by name and then excluded from the dynamic group.
  Or a **register-bound** `bound_sampler` attached to the `pipeline_layout` directly — its `binding` carries the register and space, so it needs no matching group binding.
  A sampler binding declared static this way must not also be supplied per group.
  In dx12 both become `D3D12_STATIC_SAMPLER_DESC`s the pipeline layout bakes into the root signature.
- **dynamic** — a sampler binding *not* named static is supplied per group, so each `binding_group` provides its `named_sampler` and the state can vary group to group.
  In dx12 samplers occupy their own descriptor heap and root descriptor table, so a group with dynamic samplers binds a second heap and table at dispatch.

Going register-bound means the sampler binding must leave the group layout's bindings, or the group claims that register a second time as a dynamic sampler.
`split_off_sampler_bindings` is the reflection-side split.

## Working with reflected bindings

Reflection hands you one `cc::vector<binding>` per shader stage, but a pipeline has a single binding interface.
So [`binding.hh`](../../src/shaped-graphics/binding/binding.hh) carries the two operations every multi-stage caller needs:

- `merge_bindings({stage0.bindings, stage1.bindings, …})` — the union by name, in first-seen order.
  A raytracing pipeline's global root signature must cover every stage's bindings, and a raster pipeline's group covers vertex plus fragment.
- `split_off_sampler_bindings(bindings)` — removes the sampler bindings and returns them, for the samplers bound outside the group.

## Where this is headed

Bindings are the input to the rest of the descriptor system, which consumes them (and the `raw_view`s
bound to them) to reach the GPU:

```
compiled_shader.bindings ─▶ binding_group_layout ─▶ binding_group (name → raw_view, validated) ─▶ command_list.bind + dispatch
   (reflection)              (one group's schema)     (the first raw_view consumer; backend → native descriptor)
                                     └─▶ pipeline_layout (ordered group layouts) ─▶ compute_pipeline
```

A `pipeline_layout` composes an ordered list of `binding_group_layout`s, index = bind slot, so an entire group can be rebound at one slot without disturbing the others.
It may also carry an optional **inline-constants** block — a single uniform-buffer binding, excluded from the group layouts.
That one is written directly on the command list via `cmd.compute.set_inline_constants(...)`.
Those are fast per-dispatch parameters needing no descriptor allocation — dx12 root constants, vulkan push constants.

`binding_group_layout`, `pipeline_layout` and `compute_pipeline` are schemas and PSOs, not lifetime-scoped resources.
They are built through the raw [`ctx.uncached.create_*`](../../src/shaped-graphics/context/uncached.hh) scope.
Almost always preferred is [`ctx.cached.acquire_*`](../../src/shaped-graphics/context/cached.hh), which deduplicates and builds asynchronously.
See [caches](caches.md).

A `binding_group`, being a per-instance set of bound resources, is genuinely lifetime-scoped.
`ctx.persistent.create_binding_group` for one that lives until released; `ctx.transient.create_binding_group` for per-frame scratch recycled when its epoch retires.
The recording that binds and dispatches them — `cmd.compute.bind_pipeline` / `bind_group` / `dispatch` — is lifetime-agnostic.

A `staging_binding_group` is persistent only: it exists to outlive the epoch that built it, and it is opened with `ctx.persistent.create_staging_binding_group`.
Its snapshots are persistent groups too.

The **dx12** backend implements the full chain.
A `pipeline_layout` becomes the root signature, composed from its group layouts' descriptor tables and baked static samplers.
A trailing 32-bit-constants root parameter is appended where the layout declares inline constants.
A `binding_group` allocates a range in the single shader-visible descriptor heap and translates each `raw_view` into a native CBV/SRV/UAV; a dispatch binds each slot's table and runs.
That heap is **split by lifetime**, and the two halves use different allocators because their hazard models differ:

- a leading **transient ring**, reclaimed per epoch.
  Descriptors are **written by the CPU** when a group is created and read by the GPU during that epoch, so a slot cannot be reused until the epoch that wrote it retires.
  That CPU/GPU in-flight hazard is what the ring's per-epoch watermark enforces, and it is why a transient group bound past its epoch is refused — its slots may already be reused.
  The transient *buffer* heap is unlike this: its contents are only GPU-touched, so it can bump-reset and alias across epochs — see [memory](memory.md).
- a **persistent free-ranges allocator** for the rest.
  A group's range returns to the free list when the group is released, deferred via an epoch finalizer (like buffer deletion) until its last-using epoch retires.
  So long-lived groups do not leak the heap.

The **vulkan** backend stubs the chain — each create returns a `cc::error` — until its own compute milestone.

## Deferred

Still to come on `compiled_shader`: constant-buffer member layouts, a content hash for caching, and the input/output (vertex / render-target) signatures.
Inline (root/push) constants exist on the `pipeline_layout`, but per-member payload validation against a reflected constant-buffer layout does not.
The DXC compiler reflects buffer, texture, sampler and acceleration-structure bindings.
Texel/typed buffers (`Buffer<T>` / `RWBuffer<T>`) and append/consume/counter buffers are the kinds it does not reflect.

## See also

- [binding.hh](../../src/shaped-graphics/binding/binding.hh) — `binding`, `binding_type`, `access_of` / `shape_of` / `accepts`.
- [staging_binding_group.hh](../../src/shaped-graphics/binding/staging_binding_group.hh) — the mutable builder and its `binding_slot` addressing.
- [bindless_array.hh](../../src/shaped-graphics/binding/bindless_array.hh) — view identity → element index over one array binding.
- [compiled_shader.hh](../../src/shaped-graphics/binding/compiled_shader.hh) — the shader data model.
- [views](views.md) — the bound half: `raw_view` and the typed views that convert to it.
