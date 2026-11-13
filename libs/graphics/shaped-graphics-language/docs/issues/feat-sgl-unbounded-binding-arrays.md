---
created: 2026-11-02
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/bindings.md
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-05T18:09Z
---

# Unbounded binding arrays, and arrays of samplers

A binding array is `T[N]` with `N` a literal or an option ([bindings.md "Binding arrays"](../spec/bindings.md)).
An unbounded `T[]`, an array of samplers and a multi-dimensional array are `unsupported-yet`.

An unbounded array is what a scene of unknown size binds its textures and geometry through: dx12 and vulkan have it, metal through argument buffers, and WebGPU has no binding arrays at all.
sg decides how many slots a group holds, so this has an sg side too.

## Done on 2026-11-05

`T[]` is an unbounded binding array on dx12 and vulkan (CHK-490), under `unbounded_binding_arrays`, as its binding's last member.
sg sizes it per binding group by the views it is given: dx12 at resource binding tier 3, vulkan with a variable descriptor count.
HLSL writes `name[]` (EMIT-210), slib's generated field is a `cc::vector` of views, and sg's compute test reads five buffers through one on both.
Left:

* ~~metal~~: landed 2026-11-09, as a table each group sizes behind its argument buffer, since MSL takes no unsized argument buffer member (EMIT-211).
* A binding array of more than one dimension, and an unbounded array of samplers.

## Also done on 2026-11-05

A `staging_binding_group`, and so a `bindless_array`, over an unbounded binding, given a capacity at creation: `create_staging_binding_group(layout, n)`.
sg's compute test sets five buffers of a capacity of 16, replaces one in place, and reads both snapshots on dx12 and vulkan.

An array of samplers on dx12, vulkan and metal: `sampler[N]` in SGL, `named_sampler::element` and `set_sampler_element` in sg, and a `cc::fixed_array<sg::sampler, N>` field in the generated host code.
sg's sampler test reads one texture through three samplers, through a group and through a staging group whose element 0 it replaces.

`xs.length` of an unbounded array (CHK-491): the views its group binds, which sg writes per dispatch or draw.
dx12 and vulkan read it from the reserved `sg_array_lengths`, and metal from the array's table.
sg's compute test wraps its index by it with three views and then four, on dx12 and vulkan.
