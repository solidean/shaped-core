---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/resources/gpu_resource_manager.hh
  - symbol: sv::gpu_resource_manager
---

# The bindless lock prohibits where a generation stamp would verify

`sv::gpu_resource_manager` holds the lock.
The lock is sound, but it is not what makes an index valid.
What keeps a live index from being reassigned is sg's reclaim rule — a full array reclaims only indices *not acquired this epoch* — which is structural and needs no lock.

The alternative: bump a counter on every mint and record it in `bound_resources`.
Asserting at bind time that the snapshot covers every index the recording used would *check* the invariant rather than forbid its violation.
Same cost, and a failure that names what went wrong.
