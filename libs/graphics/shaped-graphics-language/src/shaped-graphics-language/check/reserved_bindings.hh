#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics-language/check/symbols.hh>
#include <shaped-graphics-language/fwd.hh>

/// The bindings no program declares and sg binds itself, as one table: an entry here is all a new one takes.
/// `prelude_text` declares each `@internal`, so a writer meets an ordinary resource at a fixed address.
/// sg holds the other side of the contract; libs/graphics/shaped-graphics-language/docs/reserved-bindings.md is the contract, and slib checks that they agree.

/// What makes a compile reach an entry.
enum class sgl::check::reserved_use : sgl::u8
{
    /// `print`, `assert` and a robustness `report` (EMIT-183).
    diagnostics,
    /// An inline trace written as its software walk (EMIT-135).
    software_trace,
    /// The length of an unbounded binding array (CHK-491).
    array_lengths,
    /// A bare @compute, whose threads past the grid return (CHK-520).
    dispatch_grid,
    /// A trace in SGL's own SPIR-V, which reads its structure by address.
    trace_by_address,
    /// A mesh stage polyfilled as a compute pass and a draw (CHK-524).
    mesh_polyfill,
    /// A geometry stage polyfilled as a compute pass and a draw (CHK-525).
    geometry_polyfill,
    /// The tessellation stages polyfilled as compute passes and a draw (CHK-527).
    tessellation_polyfill,
};

/// Where one target finds an entry.
struct sgl::check::reserved_target
{
    /// The space on dx12, the descriptor set on vulkan, the group on WebGPU, and -1 on metal, whose entry is a parameter.
    i32 group = -1;
    /// The register on dx12, the binding on vulkan and WebGPU, and the argument table's slot on metal.
    i32 slot = -1;
    /// Bits of `stage_bit`: where this target can reach it at all.
    u16 stages = 0;
    /// Those of `stages` that also need `feature`.
    u16 stages_needing_feature = 0;
    feature needs = feature::vertex_pipeline_stores;
};

struct sgl::check::reserved_binding
{
    /// What every target spells the resource, and what sg and a reflection know it by.
    cc::string_view name;
    /// What the prelude declares, one member whose name no target writes.
    /// Empty for an entry a builtin's own text reaches, which a target declares by hand.
    cc::string_view declaration;
    reserved_use enabled_by = reserved_use::diagnostics;
    /// In the order of `emit::target`.
    reserved_target targets[4];
};

namespace sgl::check
{

/// Every stage up to and including a mesh stage, which is everything but ray tracing.
inline constexpr auto k_raster_and_compute_stages
    = u16(stage_bit(stage::vertex) | stage_bit(stage::pixel) | stage_bit(stage::compute) | stage_bit(stage::geometry)
          | stage_bit(stage::tessellation_control) | stage_bit(stage::tessellation_evaluation) | stage_bit(stage::task)
          | stage_bit(stage::mesh));
/// What vulkan writes storage from only on a device with `vertex_pipeline_stores`.
inline constexpr auto k_vertex_pipeline_stages
    = u16(stage_bit(stage::vertex) | stage_bit(stage::geometry) | stage_bit(stage::tessellation_control)
          | stage_bit(stage::tessellation_evaluation));

inline constexpr reserved_binding k_reserved_bindings[] = {
    {.name = "sg_debug",
     .declaration = "@internal binding sg_debug:\n    words: mut buffer[atomic[uint]]\n",
     .enabled_by = reserved_use::diagnostics,
     .targets = {
         {.group = 11, .slot = 0, .stages = k_every_stage},
         {.group = 3, .slot = 19, .stages = k_every_stage, .stages_needing_feature = k_vertex_pipeline_stages},
         // WebGPU's vertex stage has no writable storage
         {.group = 3, .slot = 19, .stages = u16(k_every_stage & ~stage_bit(stage::vertex))},
         // the argument table's last slot, clear of the vertex buffers from 5 up; a ray-tracing pipeline's raygen
         // takes it, and hands it every other stage through its context
         {.group = -1, .slot = 30, .stages = k_every_stage},
     }},
    {.name = "sg_acceleration_pool",
     .enabled_by = reserved_use::software_trace,
     .targets = {
         {.group = 12, .slot = 0, .stages = k_every_stage},
         {.group = 3, .slot = 17, .stages = k_every_stage},
         {.group = 3, .slot = 17, .stages = k_every_stage},
         // beside the debug window, past the vertex buffers; a ray-tracing stage takes no buffer
         {.group = -1, .slot = 28, .stages = k_raster_and_compute_stages},
     }},
    {.name = "sg_acceleration_roots",
     .enabled_by = reserved_use::software_trace,
     .targets = {
         {.group = 12, .slot = 0, .stages = k_every_stage},
         {.group = 3, .slot = 18, .stages = k_every_stage},
         {.group = 3, .slot = 18, .stages = k_every_stage},
         {.group = -1, .slot = 29, .stages = k_raster_and_compute_stages},
     }},
    // 16 lengths as sg's root constants and uniform hold them; WebGPU has no binding arrays, and metal's tables carry
    // their own count
    {.name = "sg_array_lengths",
     .declaration = "@internal binding sg_array_lengths:\n    values: uint4[4]\n",
     .enabled_by = reserved_use::array_lengths,
     .targets = {
         {.group = 12, .slot = 1, .stages = k_every_stage},
         {.group = 3, .slot = 20, .stages = k_every_stage},
         {},
         {},
     }},
    // the threads `dispatch_threads` asked for, which a bare @compute's past the grid return before (CHK-520); metal
    // dispatches exactly those threads, and needs none
    {.name = "sg_dispatch_grid",
     .declaration = "@internal binding sg_dispatch_grid:\n    threads: uint4\n",
     .enabled_by = reserved_use::dispatch_grid,
     .targets = {
         {.group = 12, .slot = 2, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 21, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 21, .stages = stage_bit(stage::compute)},
         {},
     }},
    // every acceleration member's device address by position, `uint4[8]`, two a vector, low word first: vulkan traces
    // a structure by it, so a partitioned tlas, which has no descriptor there, traces too; SGL's own SPIR-V alone reads it
    {.name = "sg_acceleration_addresses",
     .enabled_by = reserved_use::trace_by_address,
     .targets = {
         {},
         {.group = 3, .slot = 22, .stages = k_every_stage},
         {},
         {},
     }},
    // a polyfilled mesh or geometry stage's slots, which its compute form writes (CHK-524, CHK-525); metal draws every
    // mesh stage natively, and polyfills only a geometry stage
    {.name = "sg_mesh_output",
     .declaration = "@internal binding sg_mesh_output:\n    words: mut buffer[uint]\n",
     .enabled_by = reserved_use::mesh_polyfill,
     .targets = {
         {.group = 11, .slot = 1, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 23, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 23, .stages = stage_bit(stage::compute)},
         // past the vertex-input slots, which a kernel has none of
         {.group = -1, .slot = 20, .stages = stage_bit(stage::compute)},
     }},
    // the same slots, which its vertex form reads back, read-only since WebGPU's vertex stage writes no storage
    {.name = "sg_mesh_input",
     .declaration = "@internal binding sg_mesh_input:\n    words: buffer[uint]\n",
     .enabled_by = reserved_use::mesh_polyfill,
     .targets = {
         {.group = 11, .slot = 2, .stages = stage_bit(stage::vertex)},
         {.group = 3, .slot = 24, .stages = stage_bit(stage::vertex)},
         {.group = 3, .slot = 24, .stages = stage_bit(stage::vertex)},
         // past the vertex-input slots, which the vertex half binds none of
         {.group = -1, .slot = 21, .stages = stage_bit(stage::vertex)},
     }},
    // what a polyfilled geometry stage's draw was (CHK-525): `[0]` first vertex or index, primitives an instance, index
    // kind (0 none, 1 16-bit, 2 32-bit), vertex offset; `[1]` first instance, the index buffer's offset in indices;
    // `[2]` each vertex buffer's offset in words; the mesh route reads the instances at `[1].z`; a buffer rather than a
    // block, since metal has no reserved block
    {.name = "sg_geometry_draw",
     .declaration = "@internal binding sg_geometry_draw:\n    values: buffer[uint4]\n",
     .enabled_by = reserved_use::geometry_polyfill,
     .targets = {
         {.group = 11, .slot = 8, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = 3, .slot = 25, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = 3, .slot = 25, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = -1, .slot = 22, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
     }},
    // the draw's index buffer and vertex buffers as words, which a polyfilled geometry stage assembles its primitives
    // from (CHK-525)
    {.name = "sg_geometry_indices",
     .declaration = "@internal binding sg_geometry_indices:\n    words: buffer[uint]\n",
     .enabled_by = reserved_use::geometry_polyfill,
     .targets = {
         {.group = 11, .slot = 3, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = 3, .slot = 26, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = 3, .slot = 26, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = -1, .slot = 23, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
     }},
    {.name = "sg_geometry_vertices0",
     .declaration = "@internal binding sg_geometry_vertices0:\n    words: buffer[uint]\n",
     .enabled_by = reserved_use::geometry_polyfill,
     .targets = {
         {.group = 11, .slot = 4, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = 3, .slot = 27, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = 3, .slot = 27, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = -1, .slot = 24, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
     }},
    {.name = "sg_geometry_vertices1",
     .declaration = "@internal binding sg_geometry_vertices1:\n    words: buffer[uint]\n",
     .enabled_by = reserved_use::geometry_polyfill,
     .targets = {
         {.group = 11, .slot = 5, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = 3, .slot = 28, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = 3, .slot = 28, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = -1, .slot = 25, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
     }},
    {.name = "sg_geometry_vertices2",
     .declaration = "@internal binding sg_geometry_vertices2:\n    words: buffer[uint]\n",
     .enabled_by = reserved_use::geometry_polyfill,
     .targets = {
         {.group = 11, .slot = 6, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = 3, .slot = 29, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = 3, .slot = 29, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = -1, .slot = 26, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
     }},
    {.name = "sg_geometry_vertices3",
     .declaration = "@internal binding sg_geometry_vertices3:\n    words: buffer[uint]\n",
     .enabled_by = reserved_use::geometry_polyfill,
     .targets = {
         {.group = 11, .slot = 7, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = 3, .slot = 30, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = 3, .slot = 30, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
         {.group = -1, .slot = 27, .stages = u16(stage_bit(stage::compute) | stage_bit(stage::mesh))},
     }},
    // what a polyfilled tessellation draw is (CHK-527): `[0]` patches an instance, instances, partitioning, output;
    // `[1]` domain, control points a patch, words a control point, the most points a patch makes; metal binds the
    // tessellation entries where a kernel has no vertex input
    {.name = "sg_tessellation_draw",
     .declaration = "@internal binding sg_tessellation_draw:\n    values: buffer[uint4]\n",
     .enabled_by = reserved_use::tessellation_polyfill,
     .targets = {
         {.group = 11, .slot = 11, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 33, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 33, .stages = stage_bit(stage::compute)},
         {.group = -1, .slot = 10, .stages = stage_bit(stage::compute)},
     }},
    // a record of 8 words a patch: its 6 factors as the control stage returned them, then its first point and index
    {.name = "sg_tessellation_patches",
     .declaration = "@internal binding sg_tessellation_patches:\n    words: mut buffer[uint]\n",
     .enabled_by = reserved_use::tessellation_polyfill,
     .targets = {
         {.group = 11, .slot = 12, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 34, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 34, .stages = stage_bit(stage::compute)},
         {.group = -1, .slot = 11, .stages = stage_bit(stage::compute)},
     }},
    // each patch's control points as its vertex stage returned them, which the evaluation stage reads
    {.name = "sg_tessellation_controls",
     .declaration = "@internal binding sg_tessellation_controls:\n    words: mut buffer[uint]\n",
     .enabled_by = reserved_use::tessellation_polyfill,
     .targets = {
         {.group = 11, .slot = 13, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 35, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 35, .stages = stage_bit(stage::compute)},
         {.group = -1, .slot = 12, .stages = stage_bit(stage::compute)},
     }},
    // 3 words a point: u and v in 16.16, and its patch
    {.name = "sg_tessellation_points",
     .declaration = "@internal binding sg_tessellation_points:\n    words: mut buffer[uint]\n",
     .enabled_by = reserved_use::tessellation_polyfill,
     .targets = {
         {.group = 11, .slot = 14, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 36, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 36, .stages = stage_bit(stage::compute)},
         {.group = -1, .slot = 13, .stages = stage_bit(stage::compute)},
     }},
    // the draw's index buffer, each index a point
    {.name = "sg_tessellation_indices",
     .declaration = "@internal binding sg_tessellation_indices:\n    words: mut buffer[uint]\n",
     .enabled_by = reserved_use::tessellation_polyfill,
     .targets = {
         {.group = 11, .slot = 15, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 37, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 37, .stages = stage_bit(stage::compute)},
         {.group = -1, .slot = 14, .stages = stage_bit(stage::compute)},
     }},
    // the indexed draw's 5 arguments, then the evaluation pass's 3 workgroup counts, then the points in all
    {.name = "sg_tessellation_args",
     .declaration = "@internal binding sg_tessellation_args:\n    words: mut buffer[uint]\n",
     .enabled_by = reserved_use::tessellation_polyfill,
     .targets = {
         {.group = 11, .slot = 16, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 38, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 38, .stages = stage_bit(stage::compute)},
         {.group = -1, .slot = 15, .stages = stage_bit(stage::compute)},
     }},
    // a polyfilled task stage's records, one a task workgroup behind a header, which its compute form and the scan after
    // it write (CHK-524); metal draws every task stage natively
    {.name = "sg_task_output",
     .declaration = "@internal binding sg_task_output:\n    words: mut buffer[uint]\n",
     .enabled_by = reserved_use::mesh_polyfill,
     .targets = {
         {.group = 11, .slot = 9, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 31, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 31, .stages = stage_bit(stage::compute)},
         {},
     }},
    // the same records, which the mesh stage's compute form reads to find its task workgroup and its payload
    {.name = "sg_task_input",
     .declaration = "@internal binding sg_task_input:\n    words: buffer[uint]\n",
     .enabled_by = reserved_use::mesh_polyfill,
     .targets = {
         {.group = 11, .slot = 10, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 32, .stages = stage_bit(stage::compute)},
         {.group = 3, .slot = 32, .stages = stage_bit(stage::compute)},
         {},
     }},
};

/// The acceleration members `sg_acceleration_addresses` holds the address of; a trace of a later one is a native gap.
inline constexpr u32 k_acceleration_address_capacity = 16;

/// A debug window's header, in words, and the records behind it (EMIT-183).
inline constexpr i32 k_debug_cursor_word = 0;
inline constexpr i32 k_debug_capacity_word = 1;
inline constexpr i32 k_debug_dropped_word = 2;
inline constexpr i32 k_debug_header_words = 3;

/// The entry called `name`, or null.
[[nodiscard]] reserved_binding const* reserved_binding_named(cc::string_view name);

/// Whether stage `s` on `target` can reach `b`, on a device with `device`.
[[nodiscard]] bool reaches_reserved(reserved_binding const& b, emit::target target, stage s, feature_set device);
} // namespace sgl::check
