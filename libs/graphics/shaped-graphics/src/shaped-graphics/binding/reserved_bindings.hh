#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/binding/binding.hh>
#include <shaped-graphics/fwd.hh>

/// What sg binds itself, by name and by each backend's address.
/// SGL holds the other side of this contract, and slib's `reserved_contract.cc` fails the build where they differ.
/// libs/graphics/shaped-graphics-language/docs/reserved-bindings.md is the contract, and how to add an entry.
struct sg::reserved_binding_address
{
    /// What the shader declares, and what a reflection reports.
    cc::string_view name;
    /// DXC's reflection names a constant buffer after its block; empty where that is `name`.
    cc::string_view reflected_name;
    int dx12_space = -1;
    int dx12_register = -1;
    int vulkan_set = -1;
    int vulkan_binding = -1;
    int webgpu_group = -1;
    int webgpu_binding = -1;
    /// The argument table's slot; -1 where metal has no such binding.
    int metal_buffer = -1;
};

namespace sg
{

inline constexpr reserved_binding_address reserved_debug_window = {.name = "sg_debug",
                                                                   .dx12_space = 11,
                                                                   .dx12_register = 0,
                                                                   .vulkan_set = 3,
                                                                   .vulkan_binding = 19,
                                                                   .webgpu_group = 3,
                                                                   .webgpu_binding = 19,
                                                                   .metal_buffer = 30};
inline constexpr reserved_binding_address reserved_acceleration_pool = {.name = "sg_acceleration_pool",
                                                                        .dx12_space = 12,
                                                                        .dx12_register = 0,
                                                                        .vulkan_set = 3,
                                                                        .vulkan_binding = 17,
                                                                        .webgpu_group = 3,
                                                                        .webgpu_binding = 17,
                                                                        .metal_buffer = 28};
inline constexpr reserved_binding_address reserved_acceleration_roots
    = {.name = "sg_acceleration_roots",
       .reflected_name = "sg_acceleration_roots_block",
       .dx12_space = 12,
       .dx12_register = 0,
       .vulkan_set = 3,
       .vulkan_binding = 18,
       .webgpu_group = 3,
       .webgpu_binding = 18,
       .metal_buffer = 29};

/// The length of each unbounded binding array of the bound groups, which sg writes at each dispatch or draw from the
/// groups it binds; metal's argument tables carry their own, so it has none.
inline constexpr reserved_binding_address reserved_array_lengths
    = {.name = "sg_array_lengths", .dx12_space = 12, .dx12_register = 1, .vulkan_set = 3, .vulkan_binding = 20};

/// The threads `dispatch_threads` asked for, `uint4`, which a shader sg dispatches by threads alone reads to return
/// past the grid; metal dispatches exactly those threads, so it has none.
inline constexpr reserved_binding_address reserved_dispatch_grid = {.name = "sg_dispatch_grid",
                                                                    .dx12_space = 12,
                                                                    .dx12_register = 2,
                                                                    .vulkan_set = 3,
                                                                    .vulkan_binding = 21,
                                                                    .webgpu_group = 3,
                                                                    .webgpu_binding = 21};

/// Every acceleration member's device address by position, `uint4[8]`, two a vector, low word first, which vulkan writes
/// at each dispatch or draw from the groups it binds.
/// SGL's own SPIR-V traces a structure by it, which is how a partitioned tlas, with no descriptor there, is traced.
inline constexpr reserved_binding_address reserved_acceleration_addresses
    = {.name = "sg_acceleration_addresses",
       .reflected_name = "sg_acceleration_addresses_block",
       .vulkan_set = 3,
       .vulkan_binding = 22};

/// A polyfilled mesh or geometry stage's slots in 32-bit words, which its compute form writes and its vertex form reads
/// back; one buffer sg sizes per draw, bound twice.
/// metal has them for a geometry stage alone, since it draws every mesh stage natively.
/// libs/graphics/shaped-graphics/docs/concepts/mesh-polyfill.md is the protocol.
inline constexpr reserved_binding_address reserved_mesh_output = {.name = "sg_mesh_output",
                                                                  .dx12_space = 11,
                                                                  .dx12_register = 1,
                                                                  .vulkan_set = 3,
                                                                  .vulkan_binding = 23,
                                                                  .webgpu_group = 3,
                                                                  .webgpu_binding = 23,
                                                                  .metal_buffer = 20};
inline constexpr reserved_binding_address reserved_mesh_input = {.name = "sg_mesh_input",
                                                                 .dx12_space = 11,
                                                                 .dx12_register = 2,
                                                                 .vulkan_set = 3,
                                                                 .vulkan_binding = 24,
                                                                 .webgpu_group = 3,
                                                                 .webgpu_binding = 24,
                                                                 .metal_buffer = 21};

/// What a polyfilled geometry stage's draw was, `uint4[3]`, which its compute form assembles primitives by.
/// `[0]` first vertex or first index, primitives an instance, index kind (0 none, 1 16-bit, 2 32-bit), vertex offset.
/// `[1]` first instance, the index buffer's offset in indices; `[2]` each vertex buffer's offset in words.
inline constexpr reserved_binding_address reserved_geometry_draw = {.name = "sg_geometry_draw",
                                                                    .dx12_space = 11,
                                                                    .dx12_register = 8,
                                                                    .vulkan_set = 3,
                                                                    .vulkan_binding = 25,
                                                                    .webgpu_group = 3,
                                                                    .webgpu_binding = 25,
                                                                    .metal_buffer = 22};
/// The draw's index buffer and its vertex buffers in slot order, as words, which that compute form reads.
inline constexpr reserved_binding_address reserved_geometry_indices = {.name = "sg_geometry_indices",
                                                                       .dx12_space = 11,
                                                                       .dx12_register = 3,
                                                                       .vulkan_set = 3,
                                                                       .vulkan_binding = 26,
                                                                       .webgpu_group = 3,
                                                                       .webgpu_binding = 26,
                                                                       .metal_buffer = 23};
inline constexpr reserved_binding_address reserved_geometry_vertices[] = {
    {.name = "sg_geometry_vertices0",
     .dx12_space = 11,
     .dx12_register = 4,
     .vulkan_set = 3,
     .vulkan_binding = 27,
     .webgpu_group = 3,
     .webgpu_binding = 27,
     .metal_buffer = 24},
    {.name = "sg_geometry_vertices1",
     .dx12_space = 11,
     .dx12_register = 5,
     .vulkan_set = 3,
     .vulkan_binding = 28,
     .webgpu_group = 3,
     .webgpu_binding = 28,
     .metal_buffer = 25},
    {.name = "sg_geometry_vertices2",
     .dx12_space = 11,
     .dx12_register = 6,
     .vulkan_set = 3,
     .vulkan_binding = 29,
     .webgpu_group = 3,
     .webgpu_binding = 29,
     .metal_buffer = 26},
    {.name = "sg_geometry_vertices3",
     .dx12_space = 11,
     .dx12_register = 7,
     .vulkan_set = 3,
     .vulkan_binding = 30,
     .webgpu_group = 3,
     .webgpu_binding = 30,
     .metal_buffer = 27},
};

/// What a polyfilled tessellation draw is, `uint4[2]`, which its compute passes read (CHK-527).
/// Metal binds the tessellation entries at slots a kernel leaves free, having no vertex input.
inline constexpr reserved_binding_address reserved_tessellation_draw = {.name = "sg_tessellation_draw",
                                                                        .dx12_space = 11,
                                                                        .dx12_register = 11,
                                                                        .vulkan_set = 3,
                                                                        .vulkan_binding = 33,
                                                                        .webgpu_group = 3,
                                                                        .webgpu_binding = 33,
                                                                        .metal_buffer = 10};
/// A record of 8 words a patch: its factors as the control stage returned them, then its first point and index.
inline constexpr reserved_binding_address reserved_tessellation_patches = {.name = "sg_tessellation_patches",
                                                                           .dx12_space = 11,
                                                                           .dx12_register = 12,
                                                                           .vulkan_set = 3,
                                                                           .vulkan_binding = 34,
                                                                           .webgpu_group = 3,
                                                                           .webgpu_binding = 34,
                                                                           .metal_buffer = 11};
/// Each patch's control points as its vertex stage returned them, which the evaluation pass reads.
inline constexpr reserved_binding_address reserved_tessellation_controls = {.name = "sg_tessellation_controls",
                                                                            .dx12_space = 11,
                                                                            .dx12_register = 13,
                                                                            .vulkan_set = 3,
                                                                            .vulkan_binding = 35,
                                                                            .webgpu_group = 3,
                                                                            .webgpu_binding = 35,
                                                                            .metal_buffer = 12};
/// 3 words a point: u and v in 16.16, and its patch.
inline constexpr reserved_binding_address reserved_tessellation_points = {.name = "sg_tessellation_points",
                                                                          .dx12_space = 11,
                                                                          .dx12_register = 14,
                                                                          .vulkan_set = 3,
                                                                          .vulkan_binding = 36,
                                                                          .webgpu_group = 3,
                                                                          .webgpu_binding = 36,
                                                                          .metal_buffer = 13};
/// The draw's index buffer, each index a point, which the tessellation pass writes.
inline constexpr reserved_binding_address reserved_tessellation_indices = {.name = "sg_tessellation_indices",
                                                                           .dx12_space = 11,
                                                                           .dx12_register = 15,
                                                                           .vulkan_set = 3,
                                                                           .vulkan_binding = 37,
                                                                           .webgpu_group = 3,
                                                                           .webgpu_binding = 37,
                                                                           .metal_buffer = 14};
/// The indexed draw's 5 arguments, then the evaluation pass's 3 workgroup counts, then the points in all.
inline constexpr reserved_binding_address reserved_tessellation_args = {.name = "sg_tessellation_args",
                                                                        .dx12_space = 11,
                                                                        .dx12_register = 16,
                                                                        .vulkan_set = 3,
                                                                        .vulkan_binding = 38,
                                                                        .webgpu_group = 3,
                                                                        .webgpu_binding = 38,
                                                                        .metal_buffer = 15};

/// A polyfilled task stage's records, which its compute form and the scan after it write; metal draws every task
/// stage natively.
inline constexpr reserved_binding_address reserved_task_output = {.name = "sg_task_output",
                                                                  .dx12_space = 11,
                                                                  .dx12_register = 9,
                                                                  .vulkan_set = 3,
                                                                  .vulkan_binding = 31,
                                                                  .webgpu_group = 3,
                                                                  .webgpu_binding = 31};
/// The same records, which the compute half of the mesh stage it launches reads.
inline constexpr reserved_binding_address reserved_task_input = {.name = "sg_task_input",
                                                                 .dx12_space = 11,
                                                                 .dx12_register = 10,
                                                                 .vulkan_set = 3,
                                                                 .vulkan_binding = 32,
                                                                 .webgpu_group = 3,
                                                                 .webgpu_binding = 32};

inline constexpr reserved_binding_address reserved_bindings[]
    = {reserved_debug_window,          reserved_acceleration_pool,
       reserved_acceleration_roots,    reserved_array_lengths,
       reserved_dispatch_grid,         reserved_acceleration_addresses,
       reserved_mesh_output,           reserved_mesh_input,
       reserved_geometry_draw,         reserved_geometry_indices,
       reserved_geometry_vertices[0],  reserved_geometry_vertices[1],
       reserved_geometry_vertices[2],  reserved_geometry_vertices[3],
       reserved_tessellation_draw,     reserved_tessellation_patches,
       reserved_tessellation_controls, reserved_tessellation_points,
       reserved_tessellation_indices,  reserved_tessellation_args,
       reserved_task_output,           reserved_task_input};

namespace impl
{
/// The acceleration members `sg_acceleration_addresses` holds; a pipeline that binds more refuses to trace by address.
inline constexpr isize acceleration_address_capacity = 16;

/// How many unbounded binding arrays a pipeline's bound groups may hold: `uint4[4]` of 32-bit lengths, one root
/// constant each on dx12.
inline constexpr isize array_lengths_capacity = 16;

/// Whether `bindings`, a group's, end in an unbounded binding array, which is the most a group holds.
[[nodiscard]] constexpr bool has_unbounded_array(cc::span<binding const> bindings)
{
    for (auto const& b : bindings)
        if (b.is_unbounded())
            return true;
    return false;
}
} // namespace impl

/// A debug window's header, in words: where the next record goes, how many words it holds, how many records were lost.
inline constexpr int debug_cursor_word = 0;
inline constexpr int debug_capacity_word = 1;
inline constexpr int debug_dropped_word = 2;
inline constexpr int debug_header_words = 3;

/// Whether a reflection's `name` is sg's own rather than one of the shader's.
[[nodiscard]] constexpr bool is_reserved_binding_name(cc::string_view name)
{
    for (auto const& b : reserved_bindings)
        if (name == b.name || (!b.reflected_name.empty() && name == b.reflected_name))
            return true;
    return false;
}
} // namespace sg
