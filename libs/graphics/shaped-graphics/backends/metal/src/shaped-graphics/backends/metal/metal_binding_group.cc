#include "metal_binding_group.hh"

#include <shaped-graphics/backends/metal/metal_binding_layout.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

namespace sg::backend::metal
{
metal_binding_group::metal_binding_group(metal_context& ctx,
                                         metal_binding_group_layout_handle layout,
                                         MTL::Buffer* arguments,
                                         cc::vector<sg::raw_buffer_handle> bound_buffers,
                                         cc::vector<sg::raw_texture_handle> bound_textures)
  : _ctx(ctx),
    _layout(cc::move(layout)),
    _arguments(arguments),
    _bound_buffers(cc::move(bound_buffers)),
    _bound_textures(cc::move(bound_textures))
{
}

metal_binding_group::~metal_binding_group()
{
    if (_arguments == nullptr)
        return;

    auto* const arguments = _arguments;
    _arguments = nullptr;

    _ctx.residency().remove(arguments);
    _ctx.epochs().defer([arguments] { arguments->release(); });
}
} // namespace sg::backend::metal
