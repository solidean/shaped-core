#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/string/string_view.hh>
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
                                                                        .webgpu_binding = 17};
inline constexpr reserved_binding_address reserved_acceleration_roots
    = {.name = "sg_acceleration_roots",
       .reflected_name = "sg_acceleration_roots_block",
       .dx12_space = 12,
       .dx12_register = 0,
       .vulkan_set = 3,
       .vulkan_binding = 18,
       .webgpu_group = 3,
       .webgpu_binding = 18};

inline constexpr reserved_binding_address reserved_bindings[]
    = {reserved_debug_window, reserved_acceleration_pool, reserved_acceleration_roots};

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
