#include <clean-core/common/utility.hh>
#include <shaped-graphics/context.hh>
#include <shaped-graphics/context.transient.hh>

namespace sg
{
cc::result<buffer_handle> context_transient_scope::create_buffer(isize size_in_bytes, buffer_usage usage)
{
    return _ctx.create_transient_buffer(size_in_bytes, usage);
}

cc::result<binding_group_handle> context_transient_scope::create_binding_group(binding_layout_handle layout,
                                                                               cc::span<named_view const> views)
{
    return _ctx.create_binding_group(cc::move(layout), views, lifetime_scope::transient);
}
} // namespace sg
