// dx12 bind-path creation: binding_layout / compute_pipeline / binding_group. The heavy bodies live
// in the respective dx12_*.cc; these are the context create_dx12_* entry points plus the sg::context
// override forwarders (which unpack the description / downcast the abstract layout handle to the dx12 one).

#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_compute_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/compute_pipeline.hh>

namespace sg::backend::dx12
{
namespace
{
// The bind-path resources are all persistent for now; transient recycling is not implemented.
void require_persistent(sg::lifetime_scope scope)
{
    CC_ASSERT(scope == sg::lifetime_scope::persistent, "transient bind-path resources are not implemented yet");
}
} // namespace

// --- backend-typed creates -----------------------------------------------------------------------

cc::result<dx12_binding_layout_handle> dx12_context::create_dx12_binding_layout(cc::span<sg::binding const> bindings,
                                                                                sg::lifetime_scope scope)
{
    require_persistent(scope);
    return dx12_binding_layout::create(_device.Get(), bindings);
}

cc::result<dx12_compute_pipeline_handle> dx12_context::create_dx12_compute_pipeline(sg::compiled_shader const& shader,
                                                                                    dx12_binding_layout_handle layout,
                                                                                    sg::lifetime_scope scope)
{
    require_persistent(scope);
    return dx12_compute_pipeline::create(_device.Get(), cc::move(layout), shader);
}

cc::result<dx12_binding_group_handle> dx12_context::create_dx12_binding_group(dx12_binding_layout_handle layout,
                                                                              cc::span<sg::named_view const> views,
                                                                              sg::lifetime_scope scope)
{
    require_persistent(scope);
    return dx12_binding_group::create(*this, cc::move(layout), views);
}

// --- sg::context override forwarders --------------------------------------------------------------

cc::result<sg::binding_layout_handle> dx12_context::create_binding_layout(cc::span<sg::binding const> bindings,
                                                                          sg::lifetime_scope scope)
{
    return cc::result<sg::binding_layout_handle>(create_dx12_binding_layout(bindings, scope));
}

cc::result<sg::compute_pipeline_handle> dx12_context::create_compute_pipeline(sg::compute_pipeline_description const& desc,
                                                                              sg::lifetime_scope scope)
{
    auto dx = std::dynamic_pointer_cast<dx12_binding_layout const>(desc.layout);
    CC_ASSERT(dx != nullptr, "binding_layout is not a dx12 binding_layout");
    return cc::result<sg::compute_pipeline_handle>(create_dx12_compute_pipeline(desc.shader, cc::move(dx), scope));
}

cc::result<sg::binding_group_handle> dx12_context::create_binding_group(sg::binding_layout_handle layout,
                                                                        cc::span<sg::named_view const> views,
                                                                        sg::lifetime_scope scope)
{
    auto dx = std::dynamic_pointer_cast<dx12_binding_layout const>(cc::move(layout));
    CC_ASSERT(dx != nullptr, "binding_layout is not a dx12 binding_layout");
    return cc::result<sg::binding_group_handle>(create_dx12_binding_group(cc::move(dx), views, scope));
}
} // namespace sg::backend::dx12
