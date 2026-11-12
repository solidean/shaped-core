#include <shaped-graphics-language/check/reserved_bindings.hh>
#include <shaped-graphics-language/emit/emit.hh>
#include <shaped-graphics/binding/reserved_bindings.hh>

using namespace cc::primitive_defines;

// sgl and sg each hold their side of what the reserved bindings are, and neither links the other.
// slib is the one library that sees both, so a table edited on one side alone stops the build here.

namespace
{
using sgl::emit::target;

[[nodiscard]] constexpr bool agrees(sgl::check::reserved_binding const& a, sg::reserved_binding_address const& b)
{
    auto const& dx12 = a.targets[int(target::hlsl_dx12)];
    auto const& vulkan = a.targets[int(target::hlsl_vulkan)];
    auto const& wgsl = a.targets[int(target::wgsl)];
    auto const& msl = a.targets[int(target::msl)];
    return a.name == b.name && dx12.group == b.dx12_space && dx12.slot == b.dx12_register
        && vulkan.group == b.vulkan_set && vulkan.slot == b.vulkan_binding && wgsl.group == b.webgpu_group
        && wgsl.slot == b.webgpu_binding && msl.slot == b.metal_buffer;
}

[[nodiscard]] consteval bool tables_agree()
{
    constexpr auto count = sizeof(sg::reserved_bindings) / sizeof(sg::reserved_bindings[0]);
    if (sizeof(sgl::check::k_reserved_bindings) / sizeof(sgl::check::k_reserved_bindings[0]) != count)
        return false;
    for (auto i = 0u; i < count; ++i)
        if (!agrees(sgl::check::k_reserved_bindings[i], sg::reserved_bindings[i]))
            return false;
    return true;
}

static_assert(tables_agree(), "sgl's k_reserved_bindings and sg::reserved_bindings name different addresses");
static_assert(sgl::check::k_debug_cursor_word == sg::debug_cursor_word
                  && sgl::check::k_debug_capacity_word == sg::debug_capacity_word
                  && sgl::check::k_debug_dropped_word == sg::debug_dropped_word
                  && sgl::check::k_debug_header_words == sg::debug_header_words,
              "sgl writes a debug window's header where sg does not read it");
static_assert(isize(sgl::check::k_acceleration_address_capacity) == sg::impl::acceleration_address_capacity,
              "sgl reads more acceleration addresses than sg writes, or fewer");
} // namespace
