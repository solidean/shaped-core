---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_binding_group.cc
closed: 2026-11-06
resolution: fixed
---

# Every metal binding group with a TLAS allocates its own hit-offset mirror buffer

What is wrong or wanted, and what is already known.

A metal binding group that holds a TLAS allocates a shared `MTL::Buffer` of its own for the hit-offset mirror, one u64 a slot, beside its argument buffer.
It adds that buffer to residency and releases it through an epoch defer.
A ray-tracing pass that creates its groups transiently each frame so pays twice the device allocations and residency churn.

Found by a code review on 2026-11-05, not measured.
The mirror could live in the tail of the argument buffer, or in the upload ring `dispatch_rays` already stages from; measure a frame's allocations first.

## Resolution

The mirror is now the argument buffer's tail: one address per slot, written past the slots when the group is minted, by `metal_binding_create.cc` and the staging snapshot alike.
A group holding a TLAS allocates, makes resident and releases one buffer, as a group without one does; `sg metal - a binding group encodes a tlas as a resource id` pins the address.
Not measured: no caller makes transient TLAS groups per frame yet, and the change removes the second allocation outright rather than trading it.
