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
