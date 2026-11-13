---
created: 2026-11-04
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/barrier/array_declarations.hh
  - test: "sv - raster on Sponza, culled by nothing, by the view and by occlusion, with and without a depth prepass"
---

# An indirect raster draw crashes in the dx12 array-declaration tracking during the raster Sponza benchmark

`uv run dev.py benchmark "on Sponza"` (release preset, dx12, RTX 5070 Laptop) crashed twice with an access violation, in the raster Sponza benchmark that the pattern also selects.
Both times right after "sponza: 405 draws over 4 permutations", about 20 s into that benchmark; the hybrid Sponza benchmark before it had finished.

The faulting frames, innermost first:

- `sg::impl::array_declaration_book::track_declared<dx12_array_binding, ...>` (array_declarations.hh:368)
- `dx12_command_list::declare_array_accesses` (dx12_command_list.cc:570)
- `dx12_command_list::declare_raster_draw_barriers` (dx12_command_list.raster.cc:381)
- `dx12_command_list::raster_draw_indexed_indirect` (dx12_command_list.raster.cc:437)
- `sg::rendering_scope::draw_indexed_indirect` (raster.cc:343)
- `sv::raster_routine::execute` (raster_routine.cc:663 and 826), from raster-benchmark.cc:211

The raster thread had `sv.raster` open.
Not bisected; the uncommitted work at the time touched only the tracer and hybrid shaders, which the raster routine does not run.
