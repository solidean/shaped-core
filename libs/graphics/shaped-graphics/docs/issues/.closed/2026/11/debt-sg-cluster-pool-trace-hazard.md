---
created: 2026-11-12
tags: [raytracing]
refs:
  - file: libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md
closed: 2026-11-12
resolution: fixed
---

# A cluster pool rebuilt in place races a trace that still reads its clusters

A trace declares only its TLAS's storage as read, so sg orders no barrier between a trace and a later build_clusters_indirect into a pool that TLAS's cluster BLASes list.
Within one list, or across lists in flight, the rebuild may overwrite clusters a trace still reads.
The generation check catches only a tlas or partitioned tlas built after the rebuild, not one built before and traced after.
Two ways out: a tlas declares the storage of every pool its cluster BLASes list when it is bound, or hit_record_checks asserts at dispatch on a stale BLAS.
Left open by the 4b work of libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md, along with a GPU-written cluster BLAS and reading a packed record field alone.
