#include "metal_binding_group.hh"

#include <shaped-graphics/backends/metal/metal_binding_layout.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

namespace sg::backend::metal
{
metal_binding_group::metal_binding_group(metal_context& ctx,
                                         metal_binding_group_layout_handle layout,
                                         MTL::Buffer* arguments,
                                         cc::vector<bound_buffer> bound_buffers,
                                         cc::vector<bound_texture> bound_textures,
                                         cc::vector<sg::tlas_handle> bound_tlases,
                                         cc::vector<array_binding> array_bindings)
  : _ctx(ctx),
    _layout(cc::move(layout)),
    _arguments(arguments),
    _bound_buffers(cc::move(bound_buffers)),
    _bound_textures(cc::move(bound_textures)),
    _bound_tlases(cc::move(bound_tlases)),
    _array_bindings(cc::move(array_bindings))
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
