# Concept: raster (graphics) pipeline + draws

A [`raster_pipeline`](../../src/shaped-graphics/raster/raster_pipeline.hh) is the graphics counterpart of `compute_pipeline`.
It is a compiled PSO — vertex plus optional fragment shader — with its fixed-function state, built from a `raster_pipeline_description` against a `pipeline_layout`.
It is bound and drawn inside a rendering scope.
Named `raster_pipeline` (not "graphics pipeline") to match the existing `cmd.raster` recording scope and the `raster_*` command-list seams.

## The state vocabulary is backend-neutral and deliberately small

Everything the PSO needs beyond the shaders is small value structs / `enum class`es, each mapping 1:1 to DX12 and Vulkan (the trailing comment on each enumerator gives the mapping),
in the same "add when a concrete need justifies it" spirit as [`pixel_format`](../../src/shaped-graphics/resource/pixel_format.hh):

- `primitive_topology` (+ `topology_type` for the coarse PSO family) — [primitive_topology.hh](../../src/shaped-graphics/raster/primitive_topology.hh)
- `rasterization_state` — fill / cull / winding / depth-clip + a static depth bias — [rasterization_state.hh](../../src/shaped-graphics/raster/rasterization_state.hh)
- `blend_state` (per color target) + `color_write_mask` — [blend_state.hh](../../src/shaped-graphics/raster/blend_state.hh)
- `depth_stencil_state` — **reuses `compare_op` from [sampler.hh](../../src/shaped-graphics/binding/sampler.hh)**.
  [depth_stencil_state.hh](../../src/shaped-graphics/raster/depth_stencil_state.hh)
- `vertex_input_layout` — [vertex_input.hh](../../src/shaped-graphics/raster/vertex_input.hh)

The description **owns** its shaders (`compiled_shader` by value + `optional`), like `raytracing_pipeline_description`, so building on a worker thread stays safe once caching lands.

## Optional geometry / tessellation stages

Beyond the required vertex + optional fragment stage, the description carries three more optional `compiled_shader`s:
`geometry_shader`, and the tessellation pair `tessellation_control_shader` / `tessellation_evaluation_shader` (dx12 hull / domain).
Naming is backend-neutral, following the Vulkan/GL vocabulary.
dx12 maps control→hull (`hs`), evaluation→domain (`ds`) and geometry→`gs`, both in the DXC profile prefix and in the PSO's `HS` / `DS` / `GS` bytecode slots.

Tessellation constrains the topology:
the two stages are **both-or-neither**, they require `topology == primitive_topology::patch_list`, and `patch_control_points` (1..32) sets how many control points each patch carries.
`patch_list` adds a `primitive_topology_type::patch` family (PSO `PRIMITIVE_TOPOLOGY_TYPE_PATCH`);
the concrete IA topology also encodes the control-point count (`D3D_PRIMITIVE_TOPOLOGY_N_CONTROL_POINT_PATCHLIST`), computed at build and set at `bind_pipeline`.
The backend asserts these invariants.
For barrier tracking, geometry/tessellation reads fold into the `vertex` pipeline stage (as `pipeline_stage_flag` already documents), so no new draw-time hazard wiring is needed.

### Vertex input: explicit or type-driven

`vertex_input_layout` can be filled by hand (one `vertex_input_slot` per bound vertex buffer + a flat list of `vertex_attribute`s, each naming its slot),
or derived from vertex struct types with `vertex_input_layout::create<Vs...>()` — one slot per type (slot index = pack position).
Each type provides its stride and attributes through a `sg::vertex_layout_of<V>` specialization — a `static vertex_type_layout get()`.

## Target formats live in the description, not just the rendering scope

The color-target formats + per-target blend/write-mask (`color_target_state`), the depth-stencil format, and the sample count are part of `raster_pipeline_description` — not only the rendering scope —
because backends bake them into the PSO (dx12 `RTVFormats` / `DSVFormat` / `SampleDesc`; vulkan dynamic-rendering `VkPipelineRenderingCreateInfo`).
The rendering scope's bound *textures* must then match the pipeline's `color_targets` (count + format), `depth_stencil_format` and `sample_count`.
No backend checks that, and a driver need not, so **`bind_pipeline` asserts it** against `raster_pipeline::target_formats()`, which creation records.

## Draws sit on any of the raster facades

A rendering scope is opened with `cmd.raster.render_to(info)` (RAII) or `cmd.raster.manual.begin_rendering / end_rendering`.
Draw recording — `bind_pipeline`, `bind_group`, `bind_vertex_buffers` / `bind_index_buffer`, the `set_*` dynamic state, `draw` / `draw_indexed` —
lives on the `rendering_scope` RAII object that `render_to` returns, and equally on `command_list_raster_scope` (`cmd.raster`) and `command_list_raster_manual_scope` (`cmd.raster.manual`).
All three are thin forwarders to the same `command_list` `raster_*` backend seams; a draw is valid only while a scope is open (the backend asserts).

Recording through the returned scope keeps the "draw into this pass" flow on the object that opened it — and lets a routine handed only the scope record without a separate `command_list` argument.
`cmd.raster` records the same draws for a caller not holding the scope, and `cmd.raster.manual` is the path with no RAII object at all.
Only *raster* operations are mirrored onto the scope; uploads, downloads and the context stay on the command list, reached through `scope.command_list()`.

### An index fetch starts on a 4-byte boundary

`sg::index_buffer_offset_alignment` is 4.
It binds on the *sum* of the bound view's `offset_in_bytes` and the draw's `index_range.offset` — the latter counted in indices, not bytes.
So a perfectly aligned `index_buffer_view` still yields a misaligned fetch when a draw starts at an odd index of a 16-bit buffer.
That is why `bind_index_buffer` cannot carry the rule alone, and `draw_indexed` checks it too.

**It is a portable floor, hardcoded rather than queried**, the same shape as the storage-buffer offset rules in [views.md](views.md).
Metal is the only backend that minds.
It names the indices by GPU address, so sg's first index is folded into that address — and it answers a misaligned one by drawing *part* of the mesh, with no error and no validation message.
D3D12 and Vulkan take an odd first index without complaint.
A rule left to the backend that needs it would therefore be a rule nobody developing on Windows ever meets, in code that then draws wrong on a Mac.

**Every backend carries the check**, which is the convention [writing-a-backend](../writing-a-backend.md) states for contracts sg does not validate before the seam.
`tests/command_list/index_buffer_alignment-test.cc` is what holds them to it: the two refusals are invocable, so they run against whichever backends the suite has.

`sg::is_aligned_index_fetch(format, view_offset, first_index)` answers it without asserting, for a caller that would rather ask.
A mesh importer splitting sub-meshes is the case it exists for.
The two fixes are an even first index, or 32-bit indices — where every index is already 4 bytes wide and the rule cannot bind.

## Backend split

The frontend is the abstract `raster_pipeline` + description + the `raster_*` command-list virtuals.
All four backends implement it.

**dx12** fills a `D3D12_GRAPHICS_PIPELINE_STATE_DESC` in [`dx12_raster_pipeline`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_raster_pipeline.cc).
The state→D3D12 mappings live in `dx12_raster_state.cc`.
It binds on the **graphics** root-signature bind point — `SetGraphicsRootSignature` / `SetGraphicsRootDescriptorTable`, distinct from compute.
It declares vertex, index and bound-group hazards at draw time, the same rhythm as `compute_dispatch`.
[`dx12_pipeline_layout`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_pipeline_layout.cc) sets `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` on every root signature.
A graphics PSO with a vertex-input layout requires it, and it is inert for compute and ray tracing.

**vulkan** builds a `VkGraphicsPipelineCreateInfo` in [`vulkan_raster_pipeline`](../../backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_raster_pipeline.cc).
It records draws through `vkCmdBindIndexBuffer` / `vkCmdDrawIndexed`, declaring the same hazards at draw time.
A vertex attribute's SPIR-V `location` is its index in `vertex_input_layout::attributes`, since sg names an input by an HLSL semantic and SPIR-V has none.

**metal** splits what a D3D12 PSO folds together, across three MTL4 objects, and is the one backend with no `setVertexBuffer` and no root constants.
Vertex buffers and inline constants therefore arrive as addresses in the command list's one `MTL4ArgumentTable`, at the buffer indices `metal_common.hh` fixes.
Groups sit at 0 to 2, sg's reserved group at 3, inline constants at 4, and vertex-input slot `n` at 5 + `n`.
An attribute's `[[attribute(n)]]` index is its position in `attributes`, the same workaround vulkan makes for the same missing field.
[backends/metal/readme.md](../../backends/metal/readme.md) carries the rest, including why the index alignment above is a portable rule rather than metal's.

**webgpu** records through `wgpuRenderPassEncoderSetIndexBuffer` / `wgpuRenderPassEncoderDrawIndexed`, re-binding its pass state when a pass reopens.

**No backend supports an array binding on a draw.**
The raster scope has no `declare_array_*_access` pair, so a bound array binding cannot be accounted for: dx12, vulkan and metal each assert on one, and webgpu has no binding arrays at all.
See [bindings](bindings.md#array-bindings) and [TODO](../TODO.md).

## Deferred

**Indirect draws**, **dynamic** primitive topology and depth bias (baked into the PSO for now), **mesh / task** stages, and **array bindings in a draw**.
Geometry and tessellation stages are **in** (dx12). See [TODO](../TODO.md).

## See also

- [bindings](bindings.md) — the reflected bindings a `pipeline_layout` is built from.
- [views](views.md) — the render-target and depth views a rendering scope is opened on.
- [barriers](barriers.md) — the attachment transitions a rendering scope infers for you.
- [caches](caches.md) — why the raster pipeline is `ctx.uncached`-only today.
