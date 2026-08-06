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
For barrier tracking, geometry/tessellation reads fold into the `vertex` pipeline stage (as `pipeline_stage_flags` already documents), so no new draw-time hazard wiring is needed.

### Vertex input: explicit or type-driven

`vertex_input_layout` can be filled by hand (one `vertex_input_slot` per bound vertex buffer + a flat list of `vertex_attribute`s, each naming its slot),
or derived from vertex struct types with `vertex_input_layout::create<Vs...>()` — one slot per type (slot index = pack position).
Each type provides its stride and attributes through a `sg::vertex_layout_of<V>` specialization — a `static vertex_type_layout get()`.

## Target formats live in the description, not just the rendering scope

The color-target formats + per-target blend/write-mask (`color_target_state`), the depth-stencil format, and the sample count are part of `raster_pipeline_description` — not only the rendering scope —
because backends bake them into the PSO (dx12 `RTVFormats` / `DSVFormat` / `SampleDesc`; vulkan dynamic-rendering `VkPipelineRenderingCreateInfo`).
The rendering scope's bound *textures* must then match the pipeline's `color_targets` (count + format) and `depth_stencil_format`; a mismatch is a driver/debug-layer error.

## Draws sit on any of the raster facades

A rendering scope is opened with `cmd.raster.render_to(info)` (RAII) or `cmd.raster.manual.begin_rendering / end_rendering`.
Draw recording — `bind_pipeline`, `bind_group`, `bind_vertex_buffers` / `bind_index_buffer`, the `set_*` dynamic state, `draw` / `draw_indexed` —
lives on the `rendering_scope` RAII object that `render_to` returns, and equally on `command_list_raster_scope` (`cmd.raster`) and `command_list_raster_manual_scope` (`cmd.raster.manual`).
All three are thin forwarders to the same `command_list` `raster_*` backend seams; a draw is valid only while a scope is open (the backend asserts).

Recording through the returned scope keeps the "draw into this pass" flow on the object that opened it — and lets a routine handed only the scope record without a separate `command_list` argument.
`cmd.raster` records the same draws for a caller not holding the scope, and `cmd.raster.manual` is the path with no RAII object at all.
Only *raster* operations are mirrored onto the scope; uploads, downloads and the context stay on the command list, reached through `scope.command_list()`.

## Backend split (dx12 real, vulkan stubbed)

The frontend is the abstract `raster_pipeline` + description + the `raster_*` command-list virtuals.
The **dx12** backend fills a `D3D12_GRAPHICS_PIPELINE_STATE_DESC` in [`dx12_raster_pipeline`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_raster_pipeline.cc).
The state→D3D12 mappings live in `dx12_raster_state.cc`.
It binds on the **graphics** root-signature bind point — `SetGraphicsRootSignature` / `SetGraphicsRootDescriptorTable`, distinct from compute.
It declares vertex, index and bound-group hazards at draw time, the same rhythm as `compute_dispatch`.
[`dx12_pipeline_layout`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_pipeline_layout.cc) sets `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` on every root signature.
A graphics PSO with a vertex-input layout requires it, and it is inert for compute and ray tracing.
**vulkan** is a `CC_UNREACHABLE` stub.

## Deferred

PSO **caching** (`ctx.cached.acquire_raster_pipeline` + `pipeline_cache` description hashing + `async_raster_pipeline` — the compute/RT parity piece), **indirect draws**,
**dynamic** primitive topology and depth bias (baked into the PSO for now), **mesh / task** stages, and the **vulkan** implementation.
Geometry and tessellation stages are **in** (dx12). See [TODO](../TODO.md).

## See also

- [bindings](bindings.md) — the reflected bindings a `pipeline_layout` is built from.
- [views](views.md) — the render-target and depth views a rendering scope is opened on.
- [barriers](barriers.md) — the attachment transitions a rendering scope infers for you.
- [caches](caches.md) — why the raster pipeline is `ctx.uncached`-only today.
