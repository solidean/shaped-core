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
    = {.name = "sg_acceleration_addresses", .vulkan_set = 3, .vulkan_binding = 22};

inline constexpr reserved_binding_address reserved_bindings[]
    = {reserved_debug_window,  reserved_acceleration_pool, reserved_acceleration_roots,
       reserved_array_lengths, reserved_dispatch_grid,     reserved_acceleration_addresses};

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
