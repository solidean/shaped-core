// dx12_command_list raster rendering scope: transition the color / depth-stencil targets to their
// render-target / depth-stencil layouts, bind them to the output-merger, and apply each target's clear /
// discard. There is
// no graphics pipeline yet, so a scope only applies its begin-ops; draw recording lands with the pipeline.

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/container/small_vector.hh>
#include <shaped-graphics/backends/dx12/dx12_command_list.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>
#include <shaped-graphics/backends/dx12/dx12_epoch.hh>
#include <shaped-graphics/backends/dx12/dx12_texture.hh>
#include <shaped-graphics/command_list.raster.hh>
#include <shaped-graphics/pixel_format.hh>

#include <memory> // std::dynamic_pointer_cast

namespace sg::backend::dx12
{
namespace
{
[[nodiscard]] dx12_texture_handle as_dx12_texture(sg::raw_texture_handle const& tex)
{
    // Only dx12 textures ever reach a dx12 command list, so static_cast is sound; the dynamic_cast is the
    // debug-only check (stripped in release, where CC_ASSERT leaves its condition unevaluated).
    CC_ASSERT(std::dynamic_pointer_cast<dx12_texture const>(tex) != nullptr, "target texture is not a dx12 "
                                                                             "texture");
    return std::static_pointer_cast<dx12_texture const>(tex);
}
} // namespace

void dx12_command_list::raster_begin_rendering(sg::rendering_info const& info)
{
    CC_ASSERT(!_in_render_pass, "begin_rendering called while a rendering scope is already open");
    CC_ASSERT(!info.color_targets.empty() || info.depth_stencil_target.has_value(),
              "a rendering scope needs at least one color or depth-stencil target");

    // 1) Transition each target to its output layout (color -> render_target, depth -> depth_readwrite)
    //    and register it with this list. Flush the barriers up-front: enhanced barriers are illegal once the
    //    targets are bound / inside the pass, unlike a compute dispatch which flushes right before each op.
    for (auto const& ct : info.color_targets)
        track_texture_access(as_dx12_texture(ct.view.texture()), ct.view.range(), sg::pipeline_stage_flags::render_output,
                             sg::access_flags::color_write, sg::texture_layout::render_target);
    if (info.depth_stencil_target.has_value())
    {
        // The depth_readwrite layout (LAYOUT_DEPTH_STENCIL_WRITE) permits only DEPTH_STENCIL_WRITE access —
        // pairing it with depth_read is rejected. A read-only depth target (once draws exist) would use the
        // depth_readonly layout + depth_read instead.
        auto const& dt = info.depth_stencil_target.value();
        track_texture_access(as_dx12_texture(dt.view.texture()), dt.view.range(), sg::pipeline_stage_flags::render_output,
                             sg::access_flags::depth_write, sg::texture_layout::depth_readwrite);
    }
    flush_barriers();

    // 2) Create the RTV/DSV descriptors and collect the CPU handles the clears / OM bind use. The slots
    //    outlive recording and are freed against this list's epoch in end_rendering.
    cc::small_vector<D3D12_CPU_DESCRIPTOR_HANDLE, 8> rtv_handles;
    for (auto const& ct : info.color_targets)
    {
        auto rtv = _ctx.create_dx12_render_target_view(ct.view);
        CC_ASSERT(rtv.has_value(), "failed to create a render-target view (RTV heap exhausted?)");
        rtv_handles.push_back(rtv.value().handle);
        _rendering_rtv_slots.push_back(rtv.value().slot);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = {};
    if (info.depth_stencil_target.has_value())
    {
        auto dsv = _ctx.create_dx12_depth_stencil_view(info.depth_stencil_target.value().view);
        CC_ASSERT(dsv.has_value(), "failed to create a depth-stencil view (DSV heap exhausted?)");
        dsv_handle = dsv.value().handle;
        _rendering_dsv_slot = dsv.value().slot;
    }

    // 3) Apply each target's begin-op: clear to a value, discard (contents undefined), or preserve (nothing).
    for (int i = 0; i < int(info.color_targets.size()); ++i)
    {
        auto const& ct = info.color_targets[i];
        if (ct.op == sg::target_op::clear)
            _list->ClearRenderTargetView(rtv_handles[i], ct.clear_color.data, 0, nullptr);
        else if (ct.op == sg::target_op::discard)
            _list->DiscardResource(as_dx12_texture(ct.view.texture())->_resource.Get(), nullptr);
    }
    if (info.depth_stencil_target.has_value())
    {
        auto const& dt = info.depth_stencil_target.value();
        if (dt.op == sg::target_op::clear)
        {
            UINT clear_flags = D3D12_CLEAR_FLAG_DEPTH;
            if (sg::has_stencil(dt.view.format()))
                clear_flags |= D3D12_CLEAR_FLAG_STENCIL;
            _list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAGS(clear_flags), dt.clear_depth, dt.clear_stencil,
                                         0, nullptr);
        }
        else if (dt.op == sg::target_op::discard)
            _list->DiscardResource(as_dx12_texture(dt.view.texture())->_resource.Get(), nullptr);
    }

    // 4) Bind the targets to the output-merger.
    _list->OMSetRenderTargets(UINT(rtv_handles.size()), rtv_handles.empty() ? nullptr : rtv_handles.data(), FALSE,
                              info.depth_stencil_target.has_value() ? &dsv_handle : nullptr);

    // 5) Viewport + scissor — default to the full target extent (the common "render to the whole target" case).
    int extent_w = 1;
    int extent_h = 1;
    if (!info.color_targets.empty())
    {
        extent_w = info.color_targets[0].view.width();
        extent_h = info.color_targets[0].view.height();
    }
    else
    {
        extent_w = info.depth_stencil_target.value().view.width();
        extent_h = info.depth_stencil_target.value().view.height();
    }

    D3D12_VIEWPORT vp = {};
    if (info.viewport.has_value())
    {
        auto const& v = info.viewport.value();
        vp = {v.offset[0], v.offset[1], v.size[0], v.size[1], v.min_depth, v.max_depth};
    }
    else
        vp = {0.0f, 0.0f, float(extent_w), float(extent_h), 0.0f, 1.0f};
    _list->RSSetViewports(1, &vp);

    D3D12_RECT rect = {};
    if (info.scissor.has_value())
    {
        auto const& s = info.scissor.value();
        rect = {LONG(s.min[0]), LONG(s.min[1]), LONG(s.max[0]), LONG(s.max[1])};
    }
    else
        rect = {0, 0, LONG(extent_w), LONG(extent_h)};
    _list->RSSetScissorRects(1, &rect);

    _in_render_pass = true;
}

void dx12_command_list::raster_end_rendering()
{
    CC_ASSERT(_in_render_pass, "end_rendering called with no open rendering scope");

    // Unbind the targets from the output-merger: a later copy / dispatch in this list transitions them to
    // another layout, which D3D12 forbids while they are still bound as render / depth targets.
    _list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    // The RTV/DSV descriptor slots must outlive the list's GPU execution (the recorded commands read them
    // when they run). Free them against the recording epoch — this list's submit epoch — so the finalizer
    // runs only once that epoch retires, i.e. after the GPU finishes. RTV/DSV slots own no ID3D12Resource,
    // so the expiring resource carries only finalizers.
    if (!_rendering_rtv_slots.empty() || _rendering_dsv_slot != cpu_descriptor_slot::invalid)
    {
        dx12_expiring_resource expiring;
        for (auto const slot : _rendering_rtv_slots)
            expiring.finalizers.push_back([ctx = &_ctx, slot]() { ctx->free_dx12_render_target_view(slot); });
        if (_rendering_dsv_slot != cpu_descriptor_slot::invalid)
            expiring.finalizers.push_back([ctx = &_ctx, slot = _rendering_dsv_slot]()
                                          { ctx->free_dx12_depth_stencil_view(slot); });
        _ctx.schedule_deferred_deletion(cc::move(expiring));
    }

    _rendering_rtv_slots.clear();
    _rendering_dsv_slot = cpu_descriptor_slot::invalid;
    _in_render_pass = false;
}
} // namespace sg::backend::dx12
