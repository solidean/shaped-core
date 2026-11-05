---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_binding_group.cc
---

# Every metal binding group with a TLAS allocates its own hit-offset mirror buffer

What is wrong or wanted, and what is already known.

A metal binding group that holds a TLAS allocates a shared `MTL::Buffer` of its own for the hit-offset mirror, one u64 a slot, beside its argument buffer.
It adds that buffer to residency and releases it through an epoch defer.
A ray-tracing pass that creates its groups transiently each frame so pays twice the device allocations and residency churn.

Found by a code review on 2026-11-05, not measured.
The mirror could live in the tail of the argument buffer, or in the upload ring `dispatch_rays` already stages from; measure a frame's allocations first.
