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
// Layouts and pipelines are cached schemas — persistent only; a transient one is a category error.
// (binding_groups, by contrast, support both lifetimes.)
void require_persistent(sg::lifetime_scope scope)
{
    CC_ASSERT(scope == sg::lifetime_scope::persistent, "binding_layout / compute_pipeline must be persistent");
}
} // namespace

// --- backend-typed creates -----------------------------------------------------------------------

cc::result<dx12_binding_layout_handle> dx12_context::create_dx12_binding_layout(
    cc::span<sg::binding const> bindings,
    cc::span<sg::named_sampler const> static_samplers,
    sg::lifetime_scope scope)
{
    require_persistent(scope);
    return dx12_binding_layout::create(_device.Get(), bindings, static_samplers);
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
                                                                              cc::span<sg::named_sampler const> samplers,
                                                                              sg::lifetime_scope scope)
{
    return dx12_binding_group::create(*this, cc::move(layout), views, samplers, scope);
}

// --- sg::context override forwarders --------------------------------------------------------------

cc::result<sg::binding_layout_handle> dx12_context::try_create_binding_layout(cc::span<sg::binding const> bindings,
                                                                              cc::span<sg::named_sampler const> static_samplers,
                                                                              sg::lifetime_scope scope)
{
    return note_device_loss_on_error(
        cc::result<sg::binding_layout_handle>(create_dx12_binding_layout(bindings, static_samplers, scope)),
        "create_binding_layout");
}

cc::result<sg::compute_pipeline_handle> dx12_context::try_create_compute_pipeline(sg::compute_pipeline_description const& desc,
                                                                                  sg::lifetime_scope scope)
{
    auto dx = std::dynamic_pointer_cast<dx12_binding_layout const>(desc.layout);
    CC_ASSERT(dx != nullptr, "binding_layout is not a dx12 binding_layout");
    return note_device_loss_on_error(
        cc::result<sg::compute_pipeline_handle>(create_dx12_compute_pipeline(desc.shader, cc::move(dx), scope)),
        "create_compute_pipeline");
}

cc::result<sg::binding_group_handle> dx12_context::try_create_binding_group(sg::binding_layout_handle layout,
                                                                            cc::span<sg::named_view const> views,
                                                                            cc::span<sg::named_sampler const> samplers,
                                                                            sg::lifetime_scope scope)
{
    auto dx = std::dynamic_pointer_cast<dx12_binding_layout const>(cc::move(layout));
    CC_ASSERT(dx != nullptr, "binding_layout is not a dx12 binding_layout");
    // NOTE: a binding_group cc::error is usually a wiring/exhaustion failure, not device loss — but
    // consult the device anyway so a loss during descriptor writes is still recorded.
    return note_device_loss_on_error(
        cc::result<sg::binding_group_handle>(create_dx12_binding_group(cc::move(dx), views, samplers, scope)),
        "create_binding_group");
}
} // namespace sg::backend::dx12
