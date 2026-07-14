// dx12 bind-path creation: binding_group_layout / pipeline_layout / compute_pipeline / binding_group. The
// heavy bodies live in the respective dx12_*.cc; these are the context create_dx12_* entry points plus the
// sg::context override forwarders (which unpack the description / downcast the abstract layout handles).

#include <shaped-graphics/backends/dx12/dx12_binding_group.hh>
#include <shaped-graphics/backends/dx12/dx12_binding_group_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_compute_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_pipeline_layout.hh>
#include <shaped-graphics/backends/dx12/dx12_raster_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_raytracing_pipeline.hh>
#include <shaped-graphics/backends/dx12/dx12_raytracing_shader_table.hh>
#include <shaped-graphics/backends/dx12/dx12_view_desc.hh>
#include <shaped-graphics/compute_pipeline.hh>
#include <shaped-graphics/pipeline_layout.hh>
#include <shaped-graphics/raster_pipeline.hh>
#include <shaped-graphics/raytracing_pipeline.hh>
#include <shaped-graphics/raytracing_shader_table.hh>

namespace sg::backend::dx12
{
namespace
{
// Layouts and pipelines are cached schemas — persistent only; a transient one is a category error.
// (binding_groups, by contrast, support both lifetimes.)
void require_persistent(sg::lifetime_scope scope)
{
    CC_ASSERT(scope == sg::lifetime_scope::persistent, "binding_group_layout / pipeline_layout / compute_pipeline must "
                                                       "be persistent");
}
} // namespace

// --- backend-typed creates -----------------------------------------------------------------------

cc::result<dx12_binding_group_layout_handle> dx12_context::create_dx12_binding_group_layout(
    cc::span<sg::binding const> bindings,
    cc::span<sg::named_sampler const> static_samplers,
    sg::lifetime_scope scope)
{
    require_persistent(scope);
    return dx12_binding_group_layout::create(bindings, static_samplers);
}

cc::result<dx12_pipeline_layout_handle> dx12_context::create_dx12_pipeline_layout(sg::pipeline_layout_description const& desc,
                                                                                  sg::lifetime_scope scope)
{
    require_persistent(scope);
    return dx12_pipeline_layout::create(_device.Get(), desc.groups, desc.static_samplers, desc.inline_constants);
}

cc::result<dx12_compute_pipeline_handle> dx12_context::create_dx12_compute_pipeline(sg::compiled_shader const& shader,
                                                                                    dx12_pipeline_layout_handle layout,
                                                                                    cc::span<cc::byte const> cached_pipeline,
                                                                                    sg::lifetime_scope scope)
{
    require_persistent(scope);
    return dx12_compute_pipeline::create(_device.Get(), cc::move(layout), shader, cached_pipeline);
}

cc::result<dx12_raster_pipeline_handle> dx12_context::create_dx12_raster_pipeline(sg::raster_pipeline_description const& desc,
                                                                                  dx12_pipeline_layout_handle layout,
                                                                                  sg::lifetime_scope scope)
{
    require_persistent(scope);
    return dx12_raster_pipeline::create(_device.Get(), cc::move(layout), desc);
}

cc::result<dx12_raytracing_pipeline_handle> dx12_context::create_dx12_raytracing_pipeline(
    sg::raytracing_pipeline_description const& desc,
    dx12_pipeline_layout_handle layout,
    sg::lifetime_scope scope)
{
    require_persistent(scope);
    return dx12_raytracing_pipeline::create(_device.Get(), cc::move(layout), desc);
}

cc::result<dx12_raytracing_shader_table_handle> dx12_context::create_dx12_raytracing_shader_table(
    sg::raytracing_shader_table_description const& desc,
    sg::lifetime_scope scope)
{
    require_persistent(scope);
    return dx12_raytracing_shader_table::create(*this, desc);
}

cc::result<dx12_binding_group_handle> dx12_context::create_dx12_binding_group(dx12_binding_group_layout_handle layout,
                                                                              cc::span<sg::named_view const> views,
                                                                              cc::span<sg::named_sampler const> samplers,
                                                                              sg::lifetime_scope scope)
{
    return dx12_binding_group::create(*this, cc::move(layout), views, samplers, scope);
}

cc::result<dx12_descriptor_ref> dx12_context::create_dx12_render_target_view(sg::render_target_view const& view)
{
    cpu_descriptor_slot const slot = _rtv_heap->allocate();
    if (slot == cpu_descriptor_slot::invalid)
        return cc::error("render_target_view: RTV descriptor heap exhausted");
    D3D12_CPU_DESCRIPTOR_HANDLE const dst = _rtv_heap->cpu_at(slot);
    create_render_target_view(_device.Get(), view, dst);
    return dx12_descriptor_ref{.handle = dst, .slot = slot};
}

cc::result<dx12_descriptor_ref> dx12_context::create_dx12_depth_stencil_view(sg::depth_stencil_view const& view)
{
    cpu_descriptor_slot const slot = _dsv_heap->allocate();
    if (slot == cpu_descriptor_slot::invalid)
        return cc::error("depth_stencil_view: DSV descriptor heap exhausted");
    D3D12_CPU_DESCRIPTOR_HANDLE const dst = _dsv_heap->cpu_at(slot);
    create_depth_stencil_view(_device.Get(), view, dst);
    return dx12_descriptor_ref{.handle = dst, .slot = slot};
}

// --- sg::context override forwarders --------------------------------------------------------------

cc::result<sg::binding_group_layout_handle> dx12_context::try_create_binding_group_layout(
    cc::span<sg::binding const> bindings,
    cc::span<sg::named_sampler const> static_samplers,
    sg::lifetime_scope scope)
{
    return note_device_loss_on_error(
        cc::result<sg::binding_group_layout_handle>(create_dx12_binding_group_layout(bindings, static_samplers, scope)),
        "create_binding_group_layout");
}

cc::result<sg::pipeline_layout_handle> dx12_context::try_create_pipeline_layout(sg::pipeline_layout_description const& desc,
                                                                                sg::lifetime_scope scope)
{
    return note_device_loss_on_error(cc::result<sg::pipeline_layout_handle>(create_dx12_pipeline_layout(desc, scope)),
                                     "create_pipeline_layout");
}

cc::result<sg::compute_pipeline_handle> dx12_context::try_create_compute_pipeline(sg::compute_pipeline_description const& desc,
                                                                                  sg::lifetime_scope scope)
{
    auto dx = std::dynamic_pointer_cast<dx12_pipeline_layout const>(desc.layout);
    CC_ASSERT(dx != nullptr, "pipeline_layout is not a dx12 pipeline_layout");
    return note_device_loss_on_error(cc::result<sg::compute_pipeline_handle>(create_dx12_compute_pipeline(
                                         desc.shader, cc::move(dx), desc.cached_pipeline.span(), scope)),
                                     "create_compute_pipeline");
}

cc::result<sg::raster_pipeline_handle> dx12_context::try_create_raster_pipeline(sg::raster_pipeline_description const& desc,
                                                                                sg::lifetime_scope scope)
{
    auto dx = std::dynamic_pointer_cast<dx12_pipeline_layout const>(desc.layout);
    CC_ASSERT(dx != nullptr, "pipeline_layout is not a dx12 pipeline_layout");
    return note_device_loss_on_error(
        cc::result<sg::raster_pipeline_handle>(create_dx12_raster_pipeline(desc, cc::move(dx), scope)), "create_raster_"
                                                                                                        "pipeline");
}

cc::result<sg::raytracing_pipeline_handle> dx12_context::try_create_raytracing_pipeline(
    sg::raytracing_pipeline_description const& desc,
    sg::lifetime_scope scope)
{
    auto dx = std::dynamic_pointer_cast<dx12_pipeline_layout const>(desc.layout);
    CC_ASSERT(dx != nullptr, "pipeline_layout is not a dx12 pipeline_layout");
    return note_device_loss_on_error(
        cc::result<sg::raytracing_pipeline_handle>(create_dx12_raytracing_pipeline(desc, cc::move(dx), scope)),
        "create_raytracing_pipeline");
}

cc::result<sg::raytracing_shader_table_handle> dx12_context::try_create_raytracing_shader_table(
    sg::raytracing_shader_table_description const& desc,
    sg::lifetime_scope scope)
{
    return note_device_loss_on_error(
        cc::result<sg::raytracing_shader_table_handle>(create_dx12_raytracing_shader_table(desc, scope)),
        "create_raytracing_shader_table");
}

cc::result<sg::binding_group_handle> dx12_context::try_create_binding_group(sg::binding_group_layout_handle layout,
                                                                            cc::span<sg::named_view const> views,
                                                                            cc::span<sg::named_sampler const> samplers,
                                                                            sg::lifetime_scope scope)
{
    auto dx = std::dynamic_pointer_cast<dx12_binding_group_layout const>(cc::move(layout));
    CC_ASSERT(dx != nullptr, "binding_group_layout is not a dx12 binding_group_layout");
    // NOTE: a binding_group cc::error is usually a wiring/exhaustion failure, not device loss — but
    // consult the device anyway so a loss during descriptor writes is still recorded.
    return note_device_loss_on_error(
        cc::result<sg::binding_group_handle>(create_dx12_binding_group(cc::move(dx), views, samplers, scope)),
        "create_binding_group");
}
} // namespace sg::backend::dx12
