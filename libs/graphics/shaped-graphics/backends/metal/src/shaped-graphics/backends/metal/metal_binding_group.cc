#include "metal_binding_group.hh"

#include <shaped-graphics/backends/metal/metal_acceleration_structure.hh>
#include <shaped-graphics/backends/metal/metal_binding_layout.hh>
#include <shaped-graphics/backends/metal/metal_buffer.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

namespace sg::backend::metal
{
metal_binding_group::metal_binding_group(metal_context& ctx,
                                         metal_binding_group_layout_handle layout,
                                         MTL::Buffer* arguments,
                                         cc::vector<bound_buffer> bound_buffers,
                                         cc::vector<bound_texture> bound_textures,
                                         cc::vector<sg::tlas_handle> bound_tlases,
                                         cc::vector<array_binding> array_bindings,
                                         isize hit_offsets_at)
  : _ctx(ctx),
    _layout(cc::move(layout)),
    _arguments(arguments),
    _hit_offsets_at(hit_offsets_at),
    _bound_buffers(cc::move(bound_buffers)),
    _bound_textures(cc::move(bound_textures)),
    _bound_tlases(cc::move(bound_tlases)),
    _array_bindings(cc::move(array_bindings))
{
}

cc::vector<u64> metal_binding_group::hit_offsets_mirror(cc::span<sg::tlas_handle const> tlas_at_slot)
{
    auto mirror = cc::vector<u64>::create_filled(tlas_at_slot.size(), u64(0));
    auto holds_tlas = false;
    for (auto slot = isize(0); slot < tlas_at_slot.size(); ++slot)
    {
        auto const* const tlas = static_cast<metal_tlas const*>(tlas_at_slot[slot].get());
        if (tlas == nullptr)
            continue;
        holds_tlas = true;
        if (tlas->hit_group_offsets() != nullptr)
            mirror[slot] = static_cast<metal_buffer const&>(*tlas->hit_group_offsets()).gpu_address();
    }
    if (!holds_tlas)
        mirror.clear();
    return mirror;
}

metal_binding_group::~metal_binding_group()
{
    if (_arguments == nullptr)
        return;
    auto* const buffer = _arguments;
    _arguments = nullptr;
    _ctx.residency().remove(buffer);
    _ctx.epochs().defer([buffer] { buffer->release(); });
}
} // namespace sg::backend::metal
