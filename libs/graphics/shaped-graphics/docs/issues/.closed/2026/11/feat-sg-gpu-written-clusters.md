---
created: 2026-11-11
priority: next
tags: [raytracing]
refs:
  - file: libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md
closed: 2026-11-12
resolution: fixed
---

# Clusters built from records a compute pass wrote (phase 4b)

Phase 4b of libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md.
SGL's @target_layout grows two field attributes: @bits(n) packs a field into the next bits of a word, and @absent declares one an API lacks, whose store is dropped (decided).
accel.clas_triangles then declares Vulkan's 64-byte and D3D12's 80-byte CLAS records with the same fields.
sg gains a persistent cluster pool built in place, with a generation a cluster BLAS records, and an indirect CLAS build like update_indirect.
Open: a trace declares only its TLAS's storage as read, so an in-place pool rebuild races traces still in flight; the pool needs an access declaration of its own.
