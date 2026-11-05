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

* metal, through an argument buffer, which sg's metal backend does not lay out yet (EMIT-211).
* `length` of one, which no target reads back (CHK-491); the host passes it in.
* An array of samplers, and one of more than one dimension.

## Also done on 2026-11-05

A `staging_binding_group`, and so a `bindless_array`, over an unbounded binding, given a capacity at creation: `create_staging_binding_group(layout, n)`.
sg's compute test sets five buffers of a capacity of 16, replaces one in place, and reads both snapshots on dx12 and vulkan.
