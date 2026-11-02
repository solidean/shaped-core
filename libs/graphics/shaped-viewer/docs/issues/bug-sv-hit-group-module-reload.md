---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-viewer/src/shaped-viewer/resources/material_shader_cache.hh
  - symbol: sv::material_shader_cache
  - symbol: slib::compile_hit_group
---

# A permutation's hit group does not hot-reload with a module it uses

It is compiled once per permutation key, which hashes the resolution and the geometry kind.
So an edit to module `tracer` reaches the raygen and not the hit groups.
What is needed is for `slib::compile_hit_group` to report the modules it read, so the cache can hash their contents and rebuild precisely what moved.
slib's `shader_library.hh` documents the same gap from its side: `outcome.dependencies` is not returned from that seam.

This is the megakernel's alone: the wavefront dispatches a generated shade kernel per permutation instead, one compute entry point compiled again on every reload.
