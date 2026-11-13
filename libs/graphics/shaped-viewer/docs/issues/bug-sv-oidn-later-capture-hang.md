---
created: 2026-11-12
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/impl/cache_owner.hh
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/resources/gpu_resource_manager.cc
  - symbol: sv::impl::mesh_gpu_slot
---

# A later viewer on the same dx12 context can hang the GPU drawing meshes an earlier viewer placed

Several viewers in turn on one dx12 context, each placing the same `sv::asset_data`'s meshes, lost the device with `DXGI_ERROR_DEVICE_HUNG` within a few frames of a later viewer's start.
It was seen first after an OIDN capture, hence the name, but OIDN only shifted the odds.
To reproduce: `SV_FINAL_RENDER_PROBE=oidn,raw,oidn,raw,oidn,raw SV_FINAL_RENDER_MODEL=<asset> uv run dev.py test "20 second clip" --benchmarks --repeat 3`.

## Cause

`sv::mesh::cache` (and the quadric and drawing set slots) named the manager it was filled by with that manager's address.
Every viewer builds its own `gpu_resource_manager`, and the next one is often allocated where the last one was.
The slot then matched, and `_is_live` passed, because the dead manager's ids also resolve in the new one, to whatever the new one minted under them.
A mesh drew with another mesh's geometry and textures, reading past their ends, and that hung the GPU.

Everything that hid it changed the heap rather than the GPU:
- the debug layer, DRED and the sanitizer build, ASan's quarantine above all, which never hands an address straight back;
- OIDN's weight packing, a million `push_back`s, which made the address reuse likely;
- an untextured box scene, built fresh each step, so its meshes carried no cache at all.

## Fix

The slots now hold an `impl::cache_owner`, a process-unique token each manager draws once and never recycles.
`sv::mesh - a manager at a dead manager's address does not believe the mesh's cache` pins it, rebuilding a manager in the same storage.
The repro above, and a chain mixing raster, hybrid and untextured steps, passed five repeats each, where it used to fail in the first or second.
