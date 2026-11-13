---
created: 2026-11-11
tags: [raytracing]
refs:
  - file: libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md
closed: 2026-11-12
resolution: fixed
---

# dx12's half of clustered geometry (phase 4c)

Phase 4c of libs/graphics/shaped-graphics/docs/plans/clusters-and-partitioned-tlas.md.
build_clusters and build_cluster_blas on the preview runtime's ExecuteIndirectRTASOperations, compiled in the --agility-preview build.
No adapter here reports ClustersAndPTLASSupported, so it stays untested like phases 2c and 3's dx12 halves.
SGL's HLSL for dx12 already writes Shader Model 6.10's ClusterID names (EMIT-260); compiling them needs the preview DXC pin of the plan's D8, and SGL's DXIL refuses cluster_id until then.
