---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/resources/gpu_resource_manager.cc
  - symbol: sv::gpu_resource_manager::describe_instance
---

# One buffer per parameter block

That is one bindless slot per distinct (material, mesh) pairing rather than per instance, which is affordable but not free.
Packing many blocks into one buffer is invisible to the shader — it already takes an offset — so it is an optimization rather than a change of contract.
