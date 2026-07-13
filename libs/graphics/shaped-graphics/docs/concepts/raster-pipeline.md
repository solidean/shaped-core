# Concept: raster (graphics) pipeline + draws

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [bindings](bindings.md), [views](views.md), [barriers](barriers.md).

A [`raster_pipeline`](../../src/shaped-graphics/raster_pipeline.hh) is the graphics counterpart of
`compute_pipeline`: a compiled PSO (vertex + optional fragment shader) plus fixed-function state, built
from a `raster_pipeline_description` against a `pipeline_layout`. It is bound and drawn inside a rendering
scope. Named `raster_pipeline` (not "graphics pipeline") to match the existing `cmd.raster` recording
scope and the `raster_*` command-list seams.

## The state vocabulary is backend-neutral and deliberately small

Everything the PSO needs beyond the shaders is small value structs / `enum class`es, each mapping 1:1 to
DX12 and Vulkan (the trailing comment on each enumerator gives the mapping), in the same "add when a
concrete need justifies it" spirit as [`pixel_format`](../../src/shaped-graphics/pixel_format.hh):

- `primitive_topology` (+ `topology_type` for the coarse PSO family) — [primitive_topology.hh](../../src/shaped-graphics/primitive_topology.hh)
- `rasterization_state` — fill / cull / winding / depth-clip + a static depth bias — [rasterization_state.hh](../../src/shaped-graphics/rasterization_state.hh)
- `blend_state` (per color target) + `color_write_mask` — [blend_state.hh](../../src/shaped-graphics/blend_state.hh)
- `depth_stencil_state` — **reuses `compare_op` from [sampler.hh](../../src/shaped-graphics/sampler.hh)** (its doc already reserved it for the depth test) — [depth_stencil_state.hh](../../src/shaped-graphics/depth_stencil_state.hh)
- `vertex_input_layout` — [vertex_input.hh](../../src/shaped-graphics/vertex_input.hh)

The description **owns** its shaders (`compiled_shader` by value + `optional`), like
`raytracing_pipeline_description`, so building on a worker thread stays safe once caching lands.

### Vertex input: explicit or type-driven

`vertex_input_layout` can be filled by hand (one `vertex_input_slot` per bound vertex buffer + a flat list
of `vertex_attribute`s, each naming its slot), or derived from vertex struct types with
`vertex_input_layout::create<Vs...>()` — one slot per type (slot index = pack position). Each type provides
its stride + attributes through a `sg::vertex_layout_of<V>` specialization (a `static vertex_type_layout
get()`), the analogue of the prototype's per-vertex-struct descriptor.

## Target formats live in the description, not just the rendering scope

The color-target formats + per-target blend/write-mask (`color_target_state`), the depth-stencil format,
and the sample count are part of `raster_pipeline_description` — not only the rendering scope — because
backends bake them into the PSO (dx12 `RTVFormats` / `DSVFormat` / `SampleDesc`; vulkan dynamic-rendering
`VkPipelineRenderingCreateInfo`). The rendering scope's bound *textures* must then match the pipeline's
`color_targets` (count + format) and `depth_stencil_format`; a mismatch is a driver/debug-layer error.

## Draws sit on the raster facades, not the rendering-scope handle

A rendering scope is opened with `cmd.raster.render_to(info)` (RAII) or `cmd.raster.manual.begin_rendering
/ end_rendering`. Draw recording — `bind_pipeline`, `bind_group`, `bind_vertex_buffers` /
`bind_index_buffer`, the `set_*` dynamic state, `draw` / `draw_indexed` — lives on **both**
`command_list_raster_scope` (`cmd.raster`) and `command_list_raster_manual_scope` (`cmd.raster.manual`),
**not** on the `rendering_scope` RAII object, which stays a pure begin/end lifetime guard. Both facades are
thin forwarders to the same `command_list` `raster_*` backend seams; a draw is valid only while a scope is
open (the backend asserts). This keeps the RAII handle free of state and lets the manual (no-RAII-object)
path record draws through a coherent object.

## Backend split (dx12 real, vulkan stubbed)

The frontend is the abstract `raster_pipeline` + description + the `raster_*` command-list virtuals. The
**dx12** backend fills a `D3D12_GRAPHICS_PIPELINE_STATE_DESC` in
[`dx12_raster_pipeline`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_raster_pipeline.cc)
(state→D3D12 mappings in `dx12_raster_state.cc`), binds on the **graphics** root-signature bind point
(`SetGraphicsRootSignature` / `SetGraphicsRootDescriptorTable`, distinct from compute), and declares
vertex/index/bound-group hazards at draw time — the same rhythm as `compute_dispatch`. Note
[`dx12_pipeline_layout`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_pipeline_layout.cc)
now sets `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` on every root signature (required for a graphics PSO with a
vertex-input layout; inert for compute / ray tracing). **vulkan** is a `CC_UNREACHABLE` stub.

## Deferred

PSO **caching** (`ctx.cached.acquire_raster_pipeline` + `pipeline_cache` description hashing +
`async_raster_pipeline` — the compute/RT parity piece), **indirect draws**, **dynamic** primitive topology
and depth bias (baked into the PSO for now), **tessellation / geometry / mesh-task** stages, and the
**vulkan** implementation. See [TODO](../TODO.md).
